#include "rendering/shelter_map.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "core/logger.hpp"

#include "rendering/vk_shader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

constexpr VkImageSubresourceRange kDepthRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0,
                                              .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
/// The stages the map is read from: the precipitation's vertices and pixels,
/// and the probe.
constexpr VkPipelineStageFlags2 kReaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

/// Must match Push in shelter_probe.comp.glsl.
struct ProbePush {
    glm::vec4 eye;
    glm::vec4 frame;
    glm::vec4 range;
};
/// What a probe writes (shelter_probe.comp.glsl's Result).
struct ProbeResult {
    float exposure;
    float skyView;
    float written;
    float unused;
};

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
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
    b.subresourceRange = kDepthRange;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

}  // namespace

bool ShelterMap::createProbe() {
    VkDevice device = ctx_->getDevice();
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    probeSetLayout_ = createDescriptorSetLayout(device, {bindings[0], bindings[1]});
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(ProbePush)};
    probeLayout_ = createPipelineLayout(device, {probeSetLayout_}, {push});
    if (probeSetLayout_ == VK_NULL_HANDLE || probeLayout_ == VK_NULL_HANDLE) return false;

    VkShaderModule module;
    if (!module.loadFromFile(device, "assets/shaders/shelter_probe.comp.spv")) {
        LOG_ERROR("ShelterMap: could not load assets/shaders/shelter_probe.comp.spv");
        return false;
    }
    VkComputePipelineCreateInfo cpCI{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpCI.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    cpCI.layout = probeLayout_;
    const bool built = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &cpCI, nullptr,
                                                &probePipeline_) == VK_SUCCESS;
    module.destroy();
    if (!built) return false;

    const VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES_IN_FLIGHT},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &probePool_) != VK_SUCCESS) return false;

    const VkDescriptorImageInfo mapInfo{.sampler = sampler_, .imageView = map_.imageView,
                                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (Probe& p : probes_) {
        // Read on the host once the frame is done: two numbers a frame.
        p.buffer = createBuffer(ctx_->getAllocator(), sizeof(ProbeResult), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VMA_MEMORY_USAGE_CPU_ONLY);
        if (!p.buffer.buffer || !p.buffer.info.pMappedData) return false;
        std::memset(p.buffer.info.pMappedData, 0, sizeof(ProbeResult));
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = probePool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &probeSetLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &p.set) != VK_SUCCESS) return false;
        const VkDescriptorBufferInfo resultInfo{.buffer = p.buffer.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        for (auto& write : w) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = p.set;
            write.descriptorCount = 1;
        }
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &mapInfo;
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &resultInfo;
        vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
    }
    return true;
}

ShelterMap::~ShelterMap() {
    shutdown();
}

bool ShelterMap::initialize(VkContext* ctx, VkRenderPass shadowRenderPass) {
    if (!ctx) return false;
    ctx_ = ctx;
    renderPass_ = shadowRenderPass;
    VkDevice device = ctx_->getDevice();
    const auto fail = [](const char* what) {
        LOG_ERROR("ShelterMap: ", what, " - rain and snow fall through every roof");
        return false;
    };

    map_ = createImage(device, ctx_->getAllocator(), kTexels, kTexels, kShadowDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!map_.image) return fail("could not create its depth image");
    setObjectName(device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(map_.image), "shelter map");
    if (!ctx_->useDynamicRendering()) {
        VkFramebufferCreateInfo fbCI{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbCI.renderPass = renderPass_;
        fbCI.attachmentCount = 1;
        fbCI.pAttachments = &map_.imageView;
        fbCI.width = fbCI.height = kTexels;
        fbCI.layers = 1;
        if (renderPass_ == VK_NULL_HANDLE || vkCreateFramebuffer(device, &fbCI, nullptr, &framebuffer_) != VK_SUCCESS) {
            return fail("could not create its framebuffer");
        }
    }

    // Read texel by texel: a roof's edge is where it is.
    VkSamplerCreateInfo sampCI{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampCI.magFilter = VK_FILTER_NEAREST;
    sampCI.minFilter = VK_FILTER_NEAREST;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx_->getOrCreateSampler(sampCI);
    if (sampler_ == VK_NULL_HANDLE) return fail("could not create its sampler");

    if (!createProbe()) return fail("could not create its probe");

    // Open everywhere until first drawn, readable from the start: the
    // precipitation's set binds it before any frame has drawn it.
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        barrier(cmd, map_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue open{.depth = 1.0f, .stencil = 0};
        vkCmdClearDepthStencilImage(cmd, map_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &open, 1, &kDepthRange);
        barrier(cmd, map_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kReaders,
                VK_ACCESS_2_SHADER_READ_BIT);
    });
    return true;
}

void ShelterMap::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    vkDeviceWaitIdle(device);
    if (framebuffer_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer_, nullptr);
    framebuffer_ = VK_NULL_HANDLE;
    destroyImage(device, ctx_->getAllocator(), map_);
    for (Probe& p : probes_) {
        destroyBuffer(ctx_->getAllocator(), p.buffer);
        p.set = VK_NULL_HANDLE;
        p.pending = false;
    }
    destroy(device, probePool_);
    destroy(device, probePipeline_);
    destroy(device, probeLayout_);
    destroy(device, probeSetLayout_);
    sampler_ = VK_NULL_HANDLE;
    drawn_ = false;
    exposure_ = 0.0f;
    skyView_ = 1.0f;
    ctx_ = nullptr;
}

bool ShelterMap::wantsDrawing(const glm::vec3& camera, double now, uint64_t world) const {
    if (!map_.image) return false;
    if (!drawn_) return true;
    if (world != drawnWorld_ && now - drawnAt_ >= kWorldChangeSeconds) return true;
    if (glm::length(glm::vec2(camera) - frame_.centre) > 0.25f * kHalfExtent) return true;
    // The ceiling is kAbove over the camera it was drawn for; a camera that
    // has climbed most of the way to it would see roofs over it cut off.
    if (std::abs(camera.z + kAbove - frame_.ceiling) > 0.25f * kAbove) return true;
    return now - drawnAt_ >= kRefreshSeconds;
}

void ShelterMap::begin(VkCommandBuffer cmd, const glm::vec3& camera, uint64_t world) {
    drawing_ = overheadFrameFor(glm::vec2(camera), camera.z + kAbove, kHalfExtent, kRange, kTexels);
    drawingWorld_ = world;
    // The precipitation of the frames before read it, and a probe copied it.
    barrier(cmd, map_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kReaders | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, kDepthStages,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    const VkExtent2D extent{.width = kTexels, .height = kTexels};
    if (ctx_->useDynamicRendering()) {
        VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = map_.imageView;
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
        rp.framebuffer = framebuffer_;
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

void ShelterMap::end(VkCommandBuffer cmd, double now) {
    if (ctx_->useDynamicRendering()) vkCmdEndRendering(cmd);
    else vkCmdEndRenderPass(cmd);
    barrier(cmd, map_.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kDepthStages,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kReaders | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    if (!drawn_) {
        LOG_INFO("ShelterMap: first drawn round (", drawing_.centre.x, ", ", drawing_.centre.y,
                 "), ceiling at ", drawing_.ceiling);
    }
    frame_ = drawing_;
    drawnWorld_ = drawingWorld_;
    drawnAt_ = now;
    drawn_ = true;
}

void ShelterMap::probe(VkCommandBuffer cmd, uint32_t frameSlot, const glm::vec3& camera) {
    if (!drawn_ || frameSlot >= probes_.size() || probePipeline_ == VK_NULL_HANDLE) return;
    Probe& p = probes_[frameSlot];
    const ProbePush push{
        .eye = glm::vec4(camera, 2.0f * frame_.halfExtent / static_cast<float>(kTexels)),
        .frame = glm::vec4(frame_.centre, frame_.halfExtent, frame_.ceiling),
        .range = glm::vec4(frame_.range, static_cast<float>(kTexels), 0.0f, 0.0f),
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probeLayout_, 0, 1, &p.set, 0, nullptr);
    vkCmdPushConstants(cmd, probeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, 1, 1, 1);
    // For the host, once the frame's fence has passed.
    VkBufferMemoryBarrier2 host{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = p.buffer.buffer;
    host.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &host;
    cmdPipelineBarrier2(cmd, dep);
    p.pending = true;
}

void ShelterMap::collect(uint32_t frameSlot) {
    if (!ctx_ || frameSlot >= probes_.size()) return;
    Probe& p = probes_[frameSlot];
    if (!p.pending) return;
    p.pending = false;
    vmaInvalidateAllocation(ctx_->getAllocator(), p.buffer.allocation, 0, VK_WHOLE_SIZE);
    ProbeResult result{};
    std::memcpy(&result, p.buffer.info.pMappedData, sizeof(result));
    if (result.written < 0.5f) return;
    exposure_ = std::clamp(result.exposure, 0.0f, 1.0f);
    skyView_ = std::clamp(result.skyView, 0.0f, 1.0f);
}

}  // namespace rendering
}  // namespace wowee
