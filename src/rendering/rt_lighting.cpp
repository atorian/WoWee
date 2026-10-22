#include "rendering/rt_lighting.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/rt_region.hpp"
#include "rendering/rt_pass_data.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"

#include <array>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {


enum Binding : uint32_t {
    kUbo = 0,
    kSceneDepth = 1,
    kGbufStore = 2,
    kGbufCur = 3,
    kRawStore = 4,
    kRawGiStore = 5,
    kGbufPrev = 6,
    kRaw = 7,
    kRawGi = 8,
    kHistPrev = 9,
    kHistGiPrev = 10,
    kHistStore = 11,
    kHistGiStore = 12,
    kFilterIn = 13,
    kFilterInGi = 14,
    kFilterOut = 15,
    kFilterOutGi = 16,
    kReflectionStore = 17,
    kBindingCount = 18,
};

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

} // namespace

RtLighting::~RtLighting() { shutdown(); }

bool RtLighting::initialize(VkContext* ctx, RtScene* scene) {
    ctx_ = ctx;
    scene_ = scene;
    VkDevice dev = ctx_->getDevice();

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &linearSampler_) != VK_SUCCESS) return false;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev, &si, nullptr, &pointSampler_) != VK_SUCCESS) return false;

    // What the per-frame sets bind while there is no result. Its contents are
    // never read - the block's switch is off - but the binding must be valid.
    neutral_ = createImage(dev, ctx_->getAllocator(), 1, 1, kFormat,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!neutral_.image) return false;
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImageLayout(cmd, neutral_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue c{};
        c.float32[0] = c.float32[1] = 1.0f;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, neutral_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        transitionImageLayout(cmd, neutral_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_GENERAL);
    });
    return true;
}

void RtLighting::shutdown() {
    if (!ctx_) return;
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    ctx_->waitIdle();
    destroyImages();
    for (auto& s : slots_) destroyBuffer(alloc, s.ubo);
    destroyImage(dev, alloc, neutral_);
    if (depthView_) vkDestroyImageView(dev, depthView_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    for (VkPipeline* p : {&prepare_, &prepareMs_, &trace_, &temporal_, &filter_}) {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (pipelineLayout_) vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (linearSampler_) vkDestroySampler(dev, linearSampler_, nullptr);
    if (pointSampler_) vkDestroySampler(dev, pointSampler_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    linearSampler_ = pointSampler_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

bool RtLighting::createPipelines() {
    pipelinesTried_ = true;
    VkDevice dev = ctx_->getDevice();

    std::array<VkDescriptorSetLayoutBinding, kBindingCount> b{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    b[kUbo].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (uint32_t i : {kGbufStore, kRawStore, kRawGiStore, kHistStore, kHistGiStore, kFilterOut,
                       kFilterOutGi, kReflectionStore}) {
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = kBindingCount;
    li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &setLayout_) != VK_SUCCESS) return false;

    constexpr uint32_t kSets = MAX_FRAMES_IN_FLIGHT * 3;
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 10},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets * 8},
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kSets;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &pool_) != VK_SUCCESS) return false;
    for (auto& s : slots_) {
        VkDescriptorSetLayout layouts[3] = {setLayout_, setLayout_, setLayout_};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 3;
        ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(dev, &ai, s.sets) != VK_SUCCESS) return false;
        s.ubo = createBuffer(ctx_->getAllocator(), sizeof(RtPassData),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (!s.ubo.buffer) return false;
    }

    VkDescriptorSetLayout setLayouts[2] = {setLayout_, scene_->descriptorSetLayout()};
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 2;
    pl.pSetLayouts = setLayouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    auto make = [&](const char* path, VkPipeline& out) {
        VkShaderModule module;
        if (!module.loadFromFile(dev, path)) {
            LOG_ERROR("RtLighting: failed to load ", path);
            return false;
        }
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        ci.layout = pipelineLayout_;
        const bool ok = vkCreateComputePipelines(dev, ctx_->getPipelineCache(), 1, &ci, nullptr,
                                                 &out) == VK_SUCCESS;
        module.destroy();
        if (!ok) LOG_ERROR("RtLighting: failed to create the pipeline for ", path);
        return ok;
    };
    const bool hw = scene_->backend() == RtScene::Backend::Hardware;
    pipelinesOk_ = make("assets/shaders/rt_prepare.comp.spv", prepare_) &&
                   make("assets/shaders/rt_prepare_ms.comp.spv", prepareMs_) &&
                   make(hw ? "assets/shaders/rt_trace_hw.comp.spv"
                           : "assets/shaders/rt_trace.comp.spv", trace_) &&
                   make("assets/shaders/rt_temporal.comp.spv", temporal_) &&
                   make("assets/shaders/rt_filter.comp.spv", filter_);
    if (!pipelinesOk_) LOG_ERROR("RtLighting: pipelines unavailable, ray traced lighting stays off");
    return pipelinesOk_;
}

bool RtLighting::createImages(VkExtent2D e) {
    VkDevice dev = ctx_->getDevice();
    VmaAllocator alloc = ctx_->getAllocator();
    // Transfer source so a tool can read a result back (tools/rt_probe.cpp).
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto make = [&](Target& t, VkFormat format = kFormat) {
        t.img = createImage(dev, alloc, e.width, e.height, format, usage);
        return t.img.image != VK_NULL_HANDLE;
    };
    bool ok = make(raw_) && make(rawGi_) && make(tmp_) && make(tmpGi_);
    for (auto& s : slots_) {
        // Half-float distance rounds mountain positions into metre-scale bands.
        // Keep ray origins/history geometry precise; radiance can stay float16.
        ok = ok && make(s.gbuf, VK_FORMAT_R32G32B32A32_SFLOAT) &&
             make(s.hist) && make(s.histGi) && make(s.out) && make(s.outGi) && make(s.reflection);
        s.wroteImages = false;
    }
    if (!ok) return false;
    // prepare() publishes these views to the forward pass, which runs before
    // record(). Transition and clear them now: deferring this to record() left
    // the first opaque draws with descriptors pointing at UNDEFINED images.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        auto init = [&](const Target& t, bool visibility = false) {
            transitionImageLayout(cmd, t.img.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue neutral{};
            neutral.float32[0] = neutral.float32[1] = visibility ? 1.0f : 0.0f;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &neutral, 1, &range);
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = t.img.image;
            barrier.subresourceRange = range;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            cmdPipelineBarrier2(cmd, dep);
        };
        init(raw_, true);
        init(tmp_, true);
        init(rawGi_);
        init(tmpGi_);
        for (const auto& slot : slots_) {
            init(slot.hist, true);
            init(slot.out, true);
            for (const Target* t : {&slot.gbuf, &slot.histGi, &slot.outGi, &slot.reflection}) init(*t);
        }
    });
    traceExtent_ = e;
    return true;
}

void RtLighting::destroyImages() {
    DeferredResources retired(*ctx_);
    for (Target* t : {&raw_, &rawGi_, &tmp_, &tmpGi_}) retired.image(t->img);
    for (auto& s : slots_) {
        for (Target* t : {&s.gbuf, &s.hist, &s.histGi, &s.out, &s.outGi, &s.reflection}) retired.image(t->img);
        s.wroteImages = false;
        s.boundDepth = VK_NULL_HANDLE;
    }
    traceExtent_ = {};
}

bool RtLighting::prepare(VkExtent2D sceneExtent) {
    ++frameCounter_;
    if (mode_ == Mode::Off) {
        if (traceExtent_.width == 0 && !scene_->isActive()) return false;
        // Nothing of the pass or the scene's acceleration structures is kept
        // while off; the per-frame sets go back to the neutral image.
        destroyImages();
        scene_->setActive(false);
        havePrev_ = false;
        return true;
    }
    if (!pipelinesTried_ && !createPipelines()) return false;
    if (!pipelinesOk_) return false;
    scene_->setActive(true);

    const VkExtent2D want{std::max(1u, (sceneExtent.width + 1) / 2),
                          std::max(1u, (sceneExtent.height + 1) / 2)};
    if (want.width == traceExtent_.width && want.height == traceExtent_.height) return false;

    destroyImages();
    if (!createImages(want)) {
        LOG_ERROR("RtLighting: failed to create ", want.width, "x", want.height, " images");
        destroyImages();
        mode_ = Mode::Off;
        return true;
    }
    sceneExtent_ = sceneExtent;
    havePrev_ = false;
    resultFrame_ = 0;
    LOG_INFO("RtLighting: tracing at ", want.width, "x", want.height);
    return true;
}

VkImageView RtLighting::lightViewForSlot(uint32_t slot) const {
    const Slot& other = slots_[(slot + 1) % MAX_FRAMES_IN_FLIGHT];
    return other.out.img.imageView ? other.out.img.imageView : neutral_.imageView;
}

VkImageView RtLighting::reflectionViewForSlot(uint32_t slot) const {
    const Slot& other = slots_[(slot + 1) % MAX_FRAMES_IN_FLIGHT];
    return other.reflection.img.imageView ? other.reflection.img.imageView : neutral_.imageView;
}

VkImageView RtLighting::geometryViewForSlot(uint32_t slot) const {
    const Slot& other = slots_[(slot + 1) % MAX_FRAMES_IN_FLIGHT];
    return other.gbuf.img.imageView ? other.gbuf.img.imageView : neutral_.imageView;
}

VkImageView RtLighting::giViewForSlot(uint32_t slot) const {
    const Slot& other = slots_[(slot + 1) % MAX_FRAMES_IN_FLIGHT];
    return other.outGi.img.imageView ? other.outGi.img.imageView : neutral_.imageView;
}

VkImageView RtLighting::depthViewFor(VkImage image, bool msaa) {
    if (image == depthImage_ && msaa == depthViewMsaa_ && depthView_) return depthView_;
    VkDevice dev = ctx_->getDevice();
    if (depthView_) {
        VkImageView old = depthView_;
        ctx_->deferAfterAllFrameFences([dev, old]() { vkDestroyImageView(dev, old, nullptr); });
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ctx_->getDepthFormat();
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    depthView_ = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vi, nullptr, &depthView_) != VK_SUCCESS) depthView_ = VK_NULL_HANDLE;
    depthImage_ = image;
    depthViewMsaa_ = msaa;
    for (auto& s : slots_) s.boundDepth = VK_NULL_HANDLE;
    return depthView_;
}

void RtLighting::writeSlotSets(uint32_t slotIndex) {
    Slot& s = slots_[slotIndex];
    Slot& prev = slots_[(slotIndex + 1) % MAX_FRAMES_IN_FLIGHT];

    VkDescriptorBufferInfo ubo{s.ubo.buffer, 0, sizeof(RtPassData)};
    auto sampled = [&](const Target& t) {
        return VkDescriptorImageInfo{pointSampler_, t.img.imageView, VK_IMAGE_LAYOUT_GENERAL};
    };
    auto storage = [&](const Target& t) {
        return VkDescriptorImageInfo{VK_NULL_HANDLE, t.img.imageView, VK_IMAGE_LAYOUT_GENERAL};
    };
    VkDescriptorImageInfo depth{pointSampler_, depthView_,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

    // Filter passes: history -> out, out -> tmp, tmp -> out.
    const Target* filterIn[3] = {&s.hist, &s.out, &tmp_};
    const Target* filterInGi[3] = {&s.histGi, &s.outGi, &tmpGi_};
    const Target* filterOut[3] = {&s.out, &tmp_, &s.out};
    const Target* filterOutGi[3] = {&s.outGi, &tmpGi_, &s.outGi};

    for (int pass = 0; pass < 3; ++pass) {
        std::array<VkDescriptorImageInfo, kBindingCount> img{};
        img[kSceneDepth] = depth;
        img[kGbufStore] = storage(s.gbuf);
        img[kGbufCur] = sampled(s.gbuf);
        img[kRawStore] = storage(raw_);
        img[kRawGiStore] = storage(rawGi_);
        img[kReflectionStore] = storage(s.reflection);
        img[kGbufPrev] = sampled(prev.gbuf);
        img[kRaw] = sampled(raw_);
        img[kRawGi] = sampled(rawGi_);
        img[kHistPrev] = sampled(prev.hist);
        img[kHistGiPrev] = sampled(prev.histGi);
        img[kHistStore] = storage(s.hist);
        img[kHistGiStore] = storage(s.histGi);
        img[kFilterIn] = sampled(*filterIn[pass]);
        img[kFilterInGi] = sampled(*filterInGi[pass]);
        img[kFilterOut] = storage(*filterOut[pass]);
        img[kFilterOutGi] = storage(*filterOutGi[pass]);

        std::array<VkWriteDescriptorSet, kBindingCount> w{};
        for (uint32_t i = 0; i < kBindingCount; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s.sets[pass];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            if (i == kUbo) {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w[i].pBufferInfo = &ubo;
            } else {
                const bool isStorage = img[i].sampler == VK_NULL_HANDLE;
                w[i].descriptorType = isStorage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &img[i];
            }
        }
        vkUpdateDescriptorSets(ctx_->getDevice(), kBindingCount, w.data(), 0, nullptr);
    }
    s.wroteImages = true;
    s.boundDepth = depthImage_;
}

void RtLighting::record(VkCommandBuffer cmd, VkImage sceneDepth, VkExtent2D sceneExtent,
                        bool depthMsaa, const FrameInputs& in, bool sceneAlreadyPrepared) {
    if (!active() || traceExtent_.width == 0 || sceneDepth == VK_NULL_HANDLE) return;
    if (sceneExtent.width != sceneExtent_.width || sceneExtent.height != sceneExtent_.height) {
        // The scene changed size under us; prepare() resizes next frame.
        sceneExtent_ = sceneExtent;
        traceExtent_ = {};
        return;
    }
    if (!depthViewFor(sceneDepth, depthMsaa)) return;

    const uint32_t slotIndex = ctx_->getCurrentFrame();
    Slot& s = slots_[slotIndex];
    if (!s.wroteImages || s.boundDepth != depthImage_) writeSlotSets(slotIndex);

    if (!sceneAlreadyPrepared) scene_->update(cmd);
    // Separates building the scene from tracing it in the GPU timings.
    ctx_->gpuMark(cmd, RtScene::kGpuMarkLabel);

    // Last frame's slot holds history only if last frame traced into it.
    setDistance(in.distance);
    const bool havePrev = havePrev_ && resultMode_ == mode_ && resultFrame_ != 0 && resultFrame_ + 1 == frameCounter_;

    RtPassData d{};
    // Inverting projection * world-view in float loses the nearby pixel
    // differences at continent coordinates. Remove translation before the
    // multiply; all reconstruction and normal differences stay camera-relative.
    const glm::mat4 relativeViewProj = in.projection * glm::mat4(glm::mat3(in.view));
    d.invViewProj = glm::inverse(relativeViewProj);
    d.prevViewProj = havePrev ? prevViewProj_ : relativeViewProj;
    d.cameraPos = glm::vec4(in.cameraPos, static_cast<float>(frameCounter_ & 0xFFFFFF));
    d.prevCameraPos = glm::vec4(havePrev ? prevCameraPos_ : in.cameraPos, 0.0f);
    d.sunDir = glm::vec4(glm::normalize(in.sunDir), kRtSunTanRadius);
    d.sunColor = glm::vec4(in.sunColor, in.sunUp ? 1.0f : 0.0f);
    d.skyColor = glm::vec4(in.skyColor, 0.0f);
    d.params = glm::vec4(static_cast<float>(mode_), kRtAoRadius, kRtGiLength, kRtShadowLength);
    d.extent = glm::vec4(traceExtent_.width, traceExtent_.height, 1.0f / traceExtent_.width,
                         1.0f / traceExtent_.height);
    d.sceneExtent = glm::vec4(sceneExtent.width, sceneExtent.height, 1.0f / sceneExtent.width,
                              1.0f / sceneExtent.height);
    d.misc = glm::vec4(scene_->hasContent() ? 1.0f : 0.0f, havePrev ? 1.0f : 0.0f, in.traceReflections ? 1.0f : 0.0f, in.distance);
    std::memcpy(s.ubo.info.pMappedData, &d, sizeof(d));

    const uint32_t gx = (traceExtent_.width + 7) / 8;
    const uint32_t gy = (traceExtent_.height + 7) / 8;
    VkDescriptorSet sceneSet = scene_->descriptorSet();
    auto dispatch = [&](VkCommandBuffer commands, VkPipeline pipeline, int filterPass) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        VkDescriptorSet sets[2] = {s.sets[filterPass], sceneSet};
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                                0, 2, sets, 0, nullptr);
        vkCmdDispatch(commands, gx, gy, 1);
    };

    // Each image declares its actual producer and readers. The shared scratch
    // targets also have readers from the preceding submission; frame-slot
    // fences alone do not protect those images. No CPU wait is introduced.
    graph_.reset();
    const RGState computeRead{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_LAYOUT_GENERAL};
    const RGState computeWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_IMAGE_LAYOUT_GENERAL};
    const RGState surfaceRead{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_LAYOUT_GENERAL};
    const RGState depthAttachment{VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    auto target=[&](const char* name,const Target& image,RGState initial) {
        auto resource=graph_.registerResource(name);
        graph_.importImage(resource,image.img.image,initial);
        return resource;
    };
    const auto depth=graph_.registerResource("scene depth");
    graph_.importImage(depth,sceneDepth,depthAttachment,{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1});
    const auto restoredDepth=graph_.versionResource(depth,"scene depth restored");
    const auto geometry=target("current geometry",s.gbuf,surfaceRead);
    const auto raw=target("raw light",raw_,computeRead);
    const auto rawGi=target("raw GI",rawGi_,computeRead);
    const auto reflection=target("reflection",s.reflection,surfaceRead);
    const auto history=target("current history",s.hist,computeRead);
    const auto historyGi=target("current GI history",s.histGi,computeRead);
    const auto& prev=slots_[(slotIndex+1)%MAX_FRAMES_IN_FLIGHT];
    const auto oldGeometry=target("previous geometry",prev.gbuf,computeWrite);
    const auto oldHistory=target("previous history",prev.hist,computeWrite);
    const auto oldHistoryGi=target("previous GI history",prev.histGi,computeWrite);
    const auto first=target("filter first",s.out,surfaceRead);
    const auto firstGi=target("filter first GI",s.outGi,surfaceRead);
    const auto middle=target("filter middle",tmp_,computeRead);
    const auto middleGi=target("filter middle GI",tmpGi_,computeRead);
    const auto final=graph_.versionResource(first,"filter final");
    const auto finalGi=graph_.versionResource(firstGi,"filter final GI");
    const auto prepare=graph_.addPass("prepare receivers",{depth},{geometry},[&](auto commands) {
        dispatch(commands,depthMsaa?prepareMs_:prepare_,0);
    });
    graph_.use(prepare,depth,{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
    graph_.use(prepare,geometry,computeWrite);
    const auto restore=graph_.addPass("restore depth",{},{restoredDepth},[](auto) {});
    graph_.use(restore,restoredDepth,depthAttachment);
    const auto trace=graph_.addPass("trace",{geometry},{raw,rawGi,reflection},[&](auto commands) {
        dispatch(commands,trace_,0);
    });
    graph_.use(trace,geometry,computeRead);
    for(auto resource:{raw,rawGi,reflection}) graph_.use(trace,resource,computeWrite);
    const auto temporal=graph_.addPass("temporal",{geometry,raw,rawGi,oldGeometry,oldHistory,oldHistoryGi},
        {history,historyGi},[&](auto commands) { dispatch(commands,temporal_,0); });
    for(auto resource:{geometry,raw,rawGi,oldGeometry,oldHistory,oldHistoryGi}) graph_.use(temporal,resource,computeRead);
    for(auto resource:{history,historyGi}) graph_.use(temporal,resource,computeWrite);
    const RGResource sources[3][2]={{history,historyGi},{first,firstGi},{middle,middleGi}};
    const RGResource destinations[3][2]={{first,firstGi},{middle,middleGi},{final,finalGi}};
    for(int pass=0;pass<3;++pass) {
        const auto filter=graph_.addPass("filter "+std::to_string(pass),
            {geometry,sources[pass][0],sources[pass][1]}, {destinations[pass][0],destinations[pass][1]},
            [&,pass](auto commands) {
                const int32_t push[2]={1<<pass,pass==2?1:0};
                vkCmdPushConstants(commands,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
                dispatch(commands,filter_,pass);
            });
        for(auto resource:{geometry,sources[pass][0],sources[pass][1]}) graph_.use(filter,resource,computeRead);
        for(auto resource:{destinations[pass][0],destinations[pass][1]}) graph_.use(filter,resource,computeWrite);
    }
    const auto publish=graph_.addPass("publish light",{geometry,final,finalGi,reflection,history,historyGi},{},[](auto) {});
    for(auto resource:{geometry,final,finalGi,reflection,history,historyGi}) graph_.use(publish,resource,surfaceRead);
    if(!graph_.compile()) { havePrev_=false; resultFrame_=0; return; }
    graph_.record(cmd);

    prevViewProj_ = relativeViewProj;
    prevCameraPos_ = in.cameraPos;
    havePrev_ = true;
    resultFrame_ = frameCounter_;
    resultViewProj_ = relativeViewProj;
    resultCameraPos_ = in.cameraPos;
    resultMode_ = mode_;
    resultDistance_ = in.distance;
}

RtLighting::ConsumerData RtLighting::consumerData() const {
    ConsumerData c;
    if (!active() || resultFrame_ == 0 || resultFrame_ + 1 != frameCounter_) return c;
    c.viewProj = resultViewProj_;
    c.cameraPos = glm::vec4(resultCameraPos_, 1.0f);
    c.params = glm::vec4(static_cast<float>(resultMode_), resultDistance_ * 0.8f, resultDistance_, 0.0f);
    return c;
}

} // namespace rendering
} // namespace wowee
