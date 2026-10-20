#include "rendering/water_wake.hpp"
#include "rendering/glsl_as_cpp.hpp"
#include "rendering/sea_bed.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace wowee::rendering::glsl {
namespace {
// The shaders' whole includes, of which this takes the field's size and step.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../assets/shaders/ocean_spectrum.glsli"
#include "../../assets/shaders/water_wake.glsli"
#pragma GCC diagnostic pop
}
}  // namespace wowee::rendering::glsl

namespace wowee {
namespace rendering {

namespace {

constexpr uint32_t kSize = static_cast<uint32_t>(glsl::kWakeSize);
const float kTexelYards = glsl::kWakeTexelYards;
constexpr uint32_t kGroup = 16;
/// The most steps a frame takes; a frame slower than that leaves the wake
/// that much behind rather than spending the next frame catching up.
constexpr int kMostStepsPerFrame = 4;

/// What a yard of a body under the water presses on it as, in yards of
/// head: a swimmer's shoulders and back, or a wader's legs, push the water
/// into a hollow about a third as deep as they are under it.
constexpr float kWakeHeadPerYard = 0.35f;
/// How far under the surface a body still marks it.
constexpr float kWakeReach = 0.6f;

/// How long the field takes to settle unstepped: its waves fall as
/// exp(-kWakeDamping t / 2), to a twentieth in this long, by when its foam
/// (kWakeFoamSeconds to 1/e) is gone too.
const double kSettleSeconds = 2.0 * std::log(20.0) / glsl::kWakeDamping;

/// Must match WakeMovers in water_wake_passes.glsli (std140).
struct MoversGPU {
    glm::vec4 place[WaterWake::kMaxMovers];
    glm::vec4 shape[WaterWake::kMaxMovers];
    glm::vec4 head[WaterWake::kMaxMovers];
    glm::ivec4 count;
};

/// Must match WakePush in water_wake_passes.glsli.
struct WakePushGPU {
    glm::vec4 bedArea;
    glm::vec2 shoreRange;
    float along;
    float stepSeconds;
    glm::ivec2 shift;
    int32_t mode;
    int32_t vertical;
};
constexpr int32_t kLoad = 0, kDerive = 1, kStep = 2, kSurface = 3, kShiftOut = 4, kShiftIn = 5;

void computeBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
                    VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStages;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStages;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

constexpr VkPipelineStageFlags2 kWaterStages =
    VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

/// Clear `images` to still water and leave them in GENERAL: from
/// `oldLayout`, once `srcStages` are done with them.
void clearToStill(VkCommandBuffer cmd, std::initializer_list<const AllocatedImage*> images, VkImageLayout oldLayout,
                  VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess) {
    const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                        .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
    for (const AllocatedImage* img : images) {
        VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStages;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = oldLayout;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img->image;
        b.subresourceRange = range;
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        cmdPipelineBarrier2(cmd, dep);
        const VkClearColorValue still{};
        vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &still, 1, &range);
    }
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

}  // namespace

float wakeHeadFor(float bottom, float top, float surface) {
    if (top <= bottom) return 0.0f;
    // Wholly under by more than its reach: the surface does not feel it.
    if (top < surface - kWakeReach) return 0.0f;
    const float under = std::clamp(surface - bottom, 0.0f, std::min(top - bottom, 1.0f));
    // Fading as it sinks out of reach.
    const float reach = std::clamp((top - (surface - kWakeReach)) / kWakeReach, 0.0f, 1.0f);
    return kWakeHeadPerYard * under * reach;
}

WaterWake::~WaterWake() {
    shutdown();
}

bool WaterWake::initialize(VkContext* ctx, const SeaBedMap* seaBed) {
    if (!ctx || !seaBed) return false;
    ctx_ = ctx;
    seaBed_ = seaBed;
    VkDevice device = ctx_->getDevice();
    const auto fail = [](const char* what) {
        LOG_ERROR("WaterWake: ", what, " - nothing moving through the water marks it");
        return false;
    };

    surface_ = createImage(device, ctx_->getAllocator(), kSize, kSize, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    state_ = createImage(device, ctx_->getAllocator(), kSize, kSize, VK_FORMAT_R32G32B32A32_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    spectrum_ = createImage(device, ctx_->getAllocator(), kSize, kSize, VK_FORMAT_R32G32B32A32_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!surface_.image || !state_.image || !spectrum_.image) return fail("could not create its images");
    setObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(surface_.image), "water wake surface");
    setObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(state_.image), "water wake state");
    setObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(spectrum_.image), "water wake spectrum");

    VkSamplerCreateInfo sampCI{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx_->getOrCreateSampler(sampCI);
    if (sampler_ == VK_NULL_HANDLE) return fail("could not create its sampler");

    // Still water, in GENERAL for good.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        clearToStill(cmd, {&surface_, &state_, &spectrum_}, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE);
    });

    if (!createPasses()) return false;
    ready_ = true;
    return true;
}

bool WaterWake::createPasses() {
    VkDevice device = ctx_->getDevice();
    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = 5;
    layoutCI.pBindings = b;
    if (vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &setLayout_) != VK_SUCCESS) {
        LOG_ERROR("WaterWake: failed to create its set layout - nothing moving through the water marks it");
        return false;
    }
    VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(WakePushGPU)};
    VkPipelineLayoutCreateInfo plCI{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &setLayout_;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &plCI, nullptr, &layout_) != VK_SUCCESS) {
        LOG_ERROR("WaterWake: failed to create its pipeline layout - nothing moving through the water marks it");
        return false;
    }
    const auto makePipeline = [&](const char* path, VkPipeline& out) {
        VkShaderModule module;
        if (!module.loadFromFile(device, path)) {
            LOG_ERROR("WaterWake: failed to load ", path, " - nothing moving through the water marks it");
            return false;
        }
        VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = layout_;
        const bool ok = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI, nullptr, &out) == VK_SUCCESS;
        module.destroy();
        if (!ok) LOG_ERROR("WaterWake: failed to create the pipeline for ", path);
        return ok;
    };
    if (!makePipeline("assets/shaders/water_wake.comp.spv", passPipeline_) ||
        !makePipeline("assets/shaders/water_wake_fft.comp.spv", fftPipeline_)) {
        return false;
    }

    VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 4 * kFrames},
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = kFrames},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = kFrames;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR("WaterWake: failed to create its descriptor pool - nothing moving through the water marks it");
        return false;
    }
    const VkDescriptorSetLayout layouts[kFrames] = {setLayout_, setLayout_};
    VkDescriptorSetAllocateInfo setAlloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAlloc.descriptorPool = pool_;
    setAlloc.descriptorSetCount = kFrames;
    setAlloc.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &setAlloc, sets_.data()) != VK_SUCCESS) {
        LOG_ERROR("WaterWake: failed to allocate its descriptor sets - nothing moving through the water marks it");
        return false;
    }
    for (uint32_t f = 0; f < kFrames; ++f) {
        VkBufferCreateInfo bufCI{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufCI.size = sizeof(MoversGPU);
        bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapInfo{};
        if (gpuCreateBuffer(ctx_->getAllocator(), &bufCI, &allocCI, &moverBuffers_[f], &moverAllocs_[f], &mapInfo) !=
            VK_SUCCESS) {
            LOG_ERROR("WaterWake: failed to create its movers buffer - nothing moving through the water marks it");
            return false;
        }
        moverMapped_[f] = mapInfo.pMappedData;
        std::memset(moverMapped_[f], 0, sizeof(MoversGPU));

        const VkDescriptorImageInfo images[4] = {
            {.sampler = VK_NULL_HANDLE, .imageView = state_.imageView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = VK_NULL_HANDLE, .imageView = spectrum_.imageView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = VK_NULL_HANDLE, .imageView = surface_.imageView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.sampler = VK_NULL_HANDLE, .imageView = seaBed_->view(), .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        };
        const VkDescriptorBufferInfo movers{.buffer = moverBuffers_[f], .offset = 0, .range = sizeof(MoversGPU)};
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = sets_[f];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = b[i].descriptorType;
            if (i < 4) w[i].pImageInfo = &images[i];
            else w[i].pBufferInfo = &movers;
        }
        vkUpdateDescriptorSets(device, 5, w, 0, nullptr);
    }
    return true;
}

void WaterWake::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    ctx_->waitIdle();
    destroyImage(device, ctx_->getAllocator(), surface_);
    destroyImage(device, ctx_->getAllocator(), state_);
    destroyImage(device, ctx_->getAllocator(), spectrum_);
    for (uint32_t f = 0; f < kFrames; ++f) {
        destroy(ctx_->getAllocator(), moverBuffers_[f], moverAllocs_[f]);
        moverMapped_[f] = nullptr;
    }
    destroy(device, pool_);
    sets_ = {};
    destroy(device, passPipeline_);
    destroy(device, fftPipeline_);
    destroy(device, layout_);
    destroy(device, setLayout_);
    sampler_ = VK_NULL_HANDLE;
    ready_ = false;
    ctx_ = nullptr;
}

void WaterWake::rest(double deltaSeconds) {
    unseenSeconds_ += std::max(deltaSeconds, 0.0);
}

void WaterWake::advance(double deltaSeconds, glm::vec2 nearby, const std::vector<WaterMover>& movers) {
    // Back in view. Where the bodies went unseen is not a path they swam
    // this frame; and a field left long enough has settled.
    if (unseenSeconds_ > 0.0) {
        lastSeen_.clear();
        if (unseenSeconds_ >= kSettleSeconds) stillPending_ = true;
        unseenSeconds_ = 0.0;
    }

    // The field's corner, a whole number of texels from the world's, so a
    // move of the camera moves the field by whole texels and leaves its
    // waves where they were.
    const glm::ivec2 centre = glm::ivec2(glm::floor(nearby / kTexelYards));
    const glm::ivec2 corner = centre - glm::ivec2(static_cast<int>(kSize / 2));
    if (!placed_) {
        cornerTexel_ = corner;
        placed_ = true;
    } else if (corner != cornerTexel_) {
        pendingShift_ += corner - cornerTexel_;
        cornerTexel_ = corner;
    }

    pendingSeconds_ += std::max(deltaSeconds, 0.0);
    const double step = glsl::kWakeStepSeconds;
    const int steps = static_cast<int>(pendingSeconds_ / step);
    pendingSeconds_ -= steps * step;
    pendingSteps_ = std::min(pendingSteps_ + steps, kMostStepsPerFrame);

    // The bodies, in the field's yards, where they are and where they were.
    const glm::vec2 cornerYards = glm::vec2(cornerTexel_) * kTexelYards;
    std::unordered_map<uint32_t, glm::vec2> seen;
    moverCount_ = 0;
    for (const WaterMover& m : movers) {
        if (m.head <= 0.0f) continue;
        if (moverCount_ >= kMaxMovers) {
            if (!saidTooMany_) {
                saidTooMany_ = true;
                LOG_WARNING("WaterWake: more than ", kMaxMovers, " bodies in the water near the camera - "
                            "the furthest leave no wake");
            }
            break;
        }
        glm::vec2 before = m.position;
        if (auto it = lastSeen_.find(m.id); it != lastSeen_.end() && glm::length(m.position - it->second) < 10.0f) {
            before = it->second;
        }
        const glm::vec2 moved = m.position - before;
        const float length = glm::length(moved);
        const glm::vec2 way = length > 1e-3f ? moved / length : m.facing;
        const float speed = deltaSeconds > 1e-6 ? length / static_cast<float>(deltaSeconds) : 0.0f;
        place_[moverCount_] = glm::vec4(m.position - cornerYards, before - cornerYards);
        shape_[moverCount_] = glm::vec4(way, m.halfLength, m.halfWidth);
        head_[moverCount_] = glm::vec4(m.head, speed, 0.0f, 0.0f);
        ++moverCount_;
        seen[m.id] = m.position;
    }
    if (moverCount_ != lastMoverCount_) {
        LOG_INFO("WaterWake: ", moverCount_, " bodies in the water near the camera",
                 moverCount_ > 0 ? ", the nearest pressing with a head of " : "",
                 moverCount_ > 0 ? std::to_string(head_[0].x) : std::string());
        lastMoverCount_ = moverCount_;
    }
    lastSeen_ = std::move(seen);
}

glm::vec4 WaterWake::area() const {
    const glm::vec2 corner = glm::vec2(cornerTexel_) * kTexelYards;
    return {corner, 1.0f / (static_cast<float>(kSize) * kTexelYards), ran_ ? 1.0f : 0.0f};
}

void WaterWake::record(VkCommandBuffer cmd) {
    ZoneScopedN("WaterWake::record");
    if (!ready_ || cmd == VK_NULL_HANDLE || !placed_) return;
    const uint32_t frame = ctx_->getCurrentFrame() % kFrames;

    MoversGPU movers{};
    for (int i = 0; i < moverCount_; ++i) {
        movers.place[i] = place_[i];
        movers.shape[i] = shape_[i];
        movers.head[i] = head_[i];
    }
    movers.count = glm::ivec4(moverCount_, 0, 0, 0);
    std::memcpy(moverMapped_[frame], &movers, sizeof(movers));

    WakePushGPU push{};
    // Where the field's texel (0, 0) corner falls in the sea bed's map.
    const OverheadFrame& bed = seaBed_->frame();
    const glm::vec2 bedCorner = bed.centre - glm::vec2(bed.halfExtent);
    const glm::vec2 corner = glm::vec2(cornerTexel_) * kTexelYards;
    const float bedSpan = 2.0f * bed.halfExtent;
    push.bedArea = glm::vec4((corner - bedCorner) / bedSpan, kTexelYards / bedSpan, seaBed_->isDrawn() ? 1.0f : 0.0f);
    push.shoreRange = glm::vec2(bed.range, 0.0f);
    push.stepSeconds = glsl::kWakeStepSeconds;

    // The last frame's water read the surface, and its passes the rest.
    // Back after the waves would have died away: still water to go on from.
    if (stillPending_) {
        clearToStill(cmd, {&surface_, &state_, &spectrum_}, VK_IMAGE_LAYOUT_GENERAL,
                     kWaterStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        stillPending_ = false;
    }
    computeBarrier(cmd, kWaterStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    const auto between = [&] {
        computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[frame], 0, nullptr);
    const auto pass = [&](int32_t mode) {
        push.mode = mode;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, passPipeline_);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, kSize / kGroup, kSize / kGroup, 1);
        between();
    };
    const auto fft = [&] {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fftPipeline_);
        for (int vertical = 0; vertical < 2; ++vertical) {
            push.vertical = vertical;
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, 1, kSize, 1);
            between();
        }
    };

    // The field moved with the camera: what it holds, put back where it was
    // in the world; what comes in at the far edge is still water.
    if (pendingShift_ != glm::ivec2(0)) {
        push.shift = pendingShift_;
        pass(kShiftOut);
        pass(kShiftIn);
        pendingShift_ = glm::ivec2(0);
        push.shift = glm::ivec2(0);
    }
    for (int s = 0; s < pendingSteps_; ++s) {
        push.along = static_cast<float>(s + 1) / static_cast<float>(pendingSteps_);
        pass(kLoad);
        fft();
        pass(kDerive);
        fft();
        pass(kStep);
    }
    if (pendingSteps_ > 0) ran_ = true;
    // The foam the frame's steps have left, over the time they covered.
    push.stepSeconds = static_cast<float>(pendingSteps_) * glsl::kWakeStepSeconds;
    pendingSteps_ = 0;
    pass(kSurface);
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, kWaterStages,
                   VK_ACCESS_2_SHADER_READ_BIT);
}

} // namespace rendering
} // namespace wowee
