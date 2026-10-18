#include "rendering/sea_bed.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"

#include <cmath>

namespace wowee {
namespace rendering {

namespace {

constexpr VkImageSubresourceRange kDepthRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0,
                                              .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
constexpr VkImageSubresourceRange kColorRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                              .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
constexpr VkFormat kBedFormat = VK_FORMAT_R32_SFLOAT;
constexpr uint32_t kGroup = 16;  // sea_bed_combine.comp

/// The stages the water reads the map from: its vertices (tessellated or
/// not) and its pixels.
constexpr VkPipelineStageFlags2 kWaterStages =
    VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

void barrier(VkCommandBuffer cmd, VkImage image, const VkImageSubresourceRange& range,
             VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStages;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStages;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

const char* viewName(int view) {
    static const char* names[3] = {"sea bed: ground from above", "sea bed: buildings from above",
                                   "sea bed: buildings from below"};
    return names[view];
}

}  // namespace

SeaBedMap::~SeaBedMap() {
    shutdown();
}

bool SeaBedMap::initialize(VkContext* ctx, VkRenderPass shadowRenderPass) {
    if (!ctx) return false;
    ctx_ = ctx;
    renderPass_ = shadowRenderPass;
    VkDevice device = ctx_->getDevice();
    const auto fail = [](const char* what) {
        LOG_ERROR("SeaBedMap: ", what, " - no water drawn");
        return false;
    };

    for (int v = 0; v < 3; ++v) {
        views_[v] = createImage(device, ctx_->getAllocator(), kTexels, kTexels, kShadowDepthFormat,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!views_[v].image) return fail("could not create a view's depth image");
        setObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(views_[v].image), viewName(v));
        if (!ctx_->useDynamicRendering()) {
            VkFramebufferCreateInfo fbCI{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbCI.renderPass = renderPass_;
            fbCI.attachmentCount = 1;
            fbCI.pAttachments = &views_[v].imageView;
            fbCI.width = fbCI.height = kTexels;
            fbCI.layers = 1;
            if (renderPass_ == VK_NULL_HANDLE ||
                vkCreateFramebuffer(device, &fbCI, nullptr, &framebuffers_[v]) != VK_SUCCESS) {
                return fail("could not create a view's framebuffer");
            }
        }
    }
    bed_ = createImage(device, ctx_->getAllocator(), kTexels, kTexels, kBedFormat,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!bed_.image) return fail("could not create the map");
    setObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(bed_.image), "sea bed map");

    // Read texel by texel: the water and the combining pass blend themselves.
    VkSamplerCreateInfo sampCI{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampCI.magFilter = VK_FILTER_NEAREST;
    sampCI.minFilter = VK_FILTER_NEAREST;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx_->getOrCreateSampler(sampCI);
    if (sampler_ == VK_NULL_HANDLE) return fail("could not create its sampler");
    if (!createCombine()) return false;

    // The views readable for the combining pass; the map deep everywhere
    // until it is first drawn, in GENERAL for good: the water's sets bind it
    // before any frame has drawn it.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        for (const auto& v : views_) {
            barrier(cmd, v.image, kDepthRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }
        barrier(cmd, bed_.image, kColorRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue deep{};
        deep.float32[0] = 1.0f;
        vkCmdClearColorImage(cmd, bed_.image, VK_IMAGE_LAYOUT_GENERAL, &deep, 1, &kColorRange);
        barrier(cmd, bed_.image, kColorRange, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                kWaterStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    });
    return true;
}

bool SeaBedMap::createCombine() {
    VkDevice device = ctx_->getDevice();
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = 4;
    layoutCI.pBindings = b;
    VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(float)};
    VkPipelineLayoutCreateInfo plCI{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &combineSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("SeaBedMap: failed to create the combining pass's set layout - no water drawn");
        return false;
    }
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &combineSetLayout_;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &plCI, nullptr, &combineLayout_) != VK_SUCCESS) {
        LOG_ERROR("SeaBedMap: failed to create the combining pass's layout - no water drawn");
        return false;
    }
    VkShaderModule module;
    if (!module.loadFromFile(device, "assets/shaders/sea_bed_combine.comp.spv")) {
        LOG_ERROR("SeaBedMap: failed to load assets/shaders/sea_bed_combine.comp.spv - no water drawn");
        return false;
    }
    VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    cpCI.layout = combineLayout_;
    const bool built = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI, nullptr,
                                                &combinePipeline_) == VK_SUCCESS;
    module.destroy();
    if (!built) {
        LOG_ERROR("SeaBedMap: failed to create the combining pass's pipeline - no water drawn");
        return false;
    }

    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 3},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    VkDescriptorSetAllocateInfo setAlloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &combinePool_) != VK_SUCCESS) {
        LOG_ERROR("SeaBedMap: failed to create the combining pass's pool - no water drawn");
        return false;
    }
    setAlloc.descriptorPool = combinePool_;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &combineSetLayout_;
    if (vkAllocateDescriptorSets(device, &setAlloc, &combineSet_) != VK_SUCCESS) {
        LOG_ERROR("SeaBedMap: failed to allocate the combining pass's set - no water drawn");
        return false;
    }
    VkDescriptorImageInfo images[4]{};
    for (int v = 0; v < 3; ++v) {
        images[v] = {.sampler = sampler_, .imageView = views_[v].imageView,
                     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    images[3] = {.sampler = VK_NULL_HANDLE, .imageView = bed_.imageView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = combineSet_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = b[i].descriptorType;
        w[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
    return true;
}

void SeaBedMap::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    vkDeviceWaitIdle(device);
    for (VkFramebuffer& fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
        fb = VK_NULL_HANDLE;
    }
    for (auto& v : views_) destroyImage(device, ctx_->getAllocator(), v);
    destroyImage(device, ctx_->getAllocator(), bed_);
    destroy(device, combinePool_);
    combineSet_ = VK_NULL_HANDLE;
    destroy(device, combinePipeline_);
    destroy(device, combineLayout_);
    destroy(device, combineSetLayout_);
    sampler_ = VK_NULL_HANDLE;
    drawn_ = false;
    ctx_ = nullptr;
}

bool SeaBedMap::wantsDrawing(glm::vec2 nearby, float waterHeight, double now) const {
    if (!combinePipeline_) return false;
    if (!drawn_) return true;
    if (glm::length(nearby - frame_.centre) > 0.25f * kHalfExtent) return true;
    if (std::abs(waterHeight - frame_.ceiling) > 0.25f) return true;
    return now - drawnAt_ >= kRefreshSeconds;
}

void SeaBedMap::begin(glm::vec2 nearby, float waterHeight) {
    drawing_ = seaBedFrameFor(nearby, waterHeight, kHalfExtent, kRange, kTexels);
}

glm::mat4 SeaBedMap::viewMatrix(SeaBedView view) const {
    return view == SeaBedView::BuildingsFromBelow ? drawing_.viewProjectionFromBelow() : drawing_.viewProjection();
}

void SeaBedMap::beginView(VkCommandBuffer cmd, SeaBedView view) {
    const int v = static_cast<int>(view);
    // The combining pass of the last drawing read it.
    barrier(cmd, views_[v].image, kDepthRange, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    const VkExtent2D extent{.width = kTexels, .height = kTexels};
    if (ctx_->useDynamicRendering()) {
        VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = views_[v].imageView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {.depth = 1.0f, .stencil = 0};
        VkRenderingInfo info{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {.offset = {0, 0}, .extent = extent};
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);
    } else {
        VkClearValue clear{};
        clear.depthStencil = {.depth = 1.0f, .stencil = 0};
        VkRenderPassBeginInfo rp{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = renderPass_;
        rp.framebuffer = framebuffers_[v];
        rp.renderArea = {.offset = {0, 0}, .extent = extent};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    }
    const VkViewport viewport{.x = 0.0f, .y = 0.0f, .width = static_cast<float>(kTexels),
                              .height = static_cast<float>(kTexels), .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{.offset = {0, 0}, .extent = extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void SeaBedMap::endView(VkCommandBuffer cmd, SeaBedView view) {
    const int v = static_cast<int>(view);
    if (ctx_->useDynamicRendering()) vkCmdEndRendering(cmd);
    else vkCmdEndRenderPass(cmd);
    barrier(cmd, views_[v].image, kDepthRange, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

void SeaBedMap::finish(VkCommandBuffer cmd, double now) {
    // The water of the frames before read the map this overwrites.
    barrier(cmd, bed_.image, kColorRange, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            kWaterStages, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, combinePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, combineLayout_, 0, 1, &combineSet_, 0, nullptr);
    const float range = drawing_.range;
    vkCmdPushConstants(cmd, combineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(range), &range);
    vkCmdDispatch(cmd, kTexels / kGroup, kTexels / kGroup, 1);
    barrier(cmd, bed_.image, kColorRange, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            kWaterStages, VK_ACCESS_2_SHADER_READ_BIT);

    frame_ = drawing_;
    drawnAt_ = now;
    if (!drawn_) {
        LOG_INFO("SeaBedMap: first drawn round (", frame_.centre.x, ", ", frame_.centre.y,
                 "), under water at ", frame_.ceiling);
    }
    drawn_ = true;
}

} // namespace rendering
} // namespace wowee
