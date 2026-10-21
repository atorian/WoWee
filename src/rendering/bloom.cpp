#include "rendering/bloom.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <algorithm>

namespace wowee::rendering {
namespace {
constexpr VkFormat kCopy = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kLight = VK_FORMAT_R16G16B16A16_SFLOAT;

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}
}

Bloom::~Bloom() { shutdown(); }

bool Bloom::initialize(VkContext* context) {
    if (!context) return false;
    ctx_ = context;
    const auto supports = [&](VkFormat format, VkFormatFeatureFlags required) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), format, &p);
        return (p.optimalTilingFeatures & required) == required;
    };
    if (!supports(ctx_->getSwapchainFormat(), VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !supports(kCopy, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ||
        !supports(kLight, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_WARNING("Bloom: required copy/filter/storage image formats unsupported - glow unavailable");
        return false;
    }
    const VkDevice device = ctx_->getDevice();
    VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx_->getOrCreateSampler(sampler);
    downsampleSetLayout_ = createDescriptorSetLayout(device, {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
    compositeSetLayout_ = createDescriptorSetLayout(device, {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kLevels, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    if (!sampler_ || !downsampleSetLayout_ || !compositeSetLayout_) {
        LOG_ERROR("Bloom: failed to create sampler/descriptor layouts - glow unavailable");
        return false;
    }
    downsampleLayout_ = createPipelineLayout(device, {downsampleSetLayout_}, {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int)}});
    compositeLayout_ = createPipelineLayout(device, {compositeSetLayout_}, {{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)}});
    if (!downsampleLayout_ || !compositeLayout_) {
        LOG_ERROR("Bloom: failed to create pipeline layouts - glow unavailable");
        return false;
    }
    VkShaderModule shader;
    if (!shader.loadFromFile(device, "assets/shaders/bloom_downsample.comp.spv")) {
        LOG_ERROR("Bloom: could not load assets/shaders/bloom_downsample.comp.spv - glow unavailable");
        return false;
    }
    VkComputePipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = shader.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
    pipeline.layout = downsampleLayout_;
    const VkResult result = vkCreateComputePipelines(device, ctx_->getPipelineCache(), 1, &pipeline, nullptr, &downsample_);
    shader.destroy();
    if (result != VK_SUCCESS) {
        LOG_ERROR("Bloom: compute pipeline creation failed (", result, ") - glow unavailable");
        return false;
    }
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames * kLevels * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFrames * kLevels}};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kFrames * (kLevels + 1);
    pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR("Bloom: descriptor pool creation failed - glow unavailable");
        return false;
    }
    for (auto& frame : frames_) {
        VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_; allocate.descriptorSetCount = 1;
        for (auto& set : frame.downsample) {
            allocate.pSetLayouts = &downsampleSetLayout_;
            if (vkAllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) {
                LOG_ERROR("Bloom: downsample descriptor allocation failed - glow unavailable");
                return false;
            }
        }
        allocate.pSetLayouts = &compositeSetLayout_;
        if (vkAllocateDescriptorSets(device, &allocate, &frame.composite) != VK_SUCCESS) {
            LOG_ERROR("Bloom: composite descriptor allocation failed - glow unavailable");
            return false;
        }
    }
    if (!ensureComposite()) return false;
    usable_ = true;
    LOG_INFO("Bloom: five-scale scene glow initialized (LDR source, before UI)");
    return true;
}

bool Bloom::ensureComposite() {
    if (composite_ && compositeFormat_ == ctx_->getSwapchainFormat()) return true;
    DeferredResources retired(*ctx_);
    retired.destroy(composite_);
    const auto shaders = loadShaderPair(ctx_->getDevice(), "assets/shaders/postprocess.vert.spv",
                                       "assets/shaders/bloom_composite.frag.spv", "bloom");
    if (!shaders) {
        LOG_ERROR("Bloom: composite shaders could not load - glow unavailable");
        return false;
    }
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    composite_ = PipelineBuilder().setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({}, {}).setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE).setNoDepthTest()
        .setColorBlendAttachment(blend).setMultisample(VK_SAMPLE_COUNT_1_BIT)
        .setLayout(compositeLayout_).setRenderPass(ctx_->getOverlayRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(ctx_->getDevice(), ctx_->getPipelineCache());
    if (!composite_) {
        LOG_ERROR("Bloom: composite pipeline creation failed - glow unavailable");
        return false;
    }
    compositeFormat_ = ctx_->getSwapchainFormat();
    return true;
}

void Bloom::retireTargets(Frame& frame) {
    DeferredResources retired(*ctx_);
    retired.image(frame.source);
    for (auto& level : frame.levels) retired.image(level);
    frame.ready = false;
}

bool Bloom::ensureTargets(Frame& frame, VkExtent2D extent) {
    if (frame.source.image && frame.source.extent.width == extent.width && frame.source.extent.height == extent.height)
        return true;
    retireTargets(frame);
    frame.source = createImage(ctx_->getDevice(), ctx_->getAllocator(), extent.width, extent.height, kCopy,
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    for (auto& level : frame.levels) {
        extent = {std::max(1u, (extent.width + 1) / 2), std::max(1u, (extent.height + 1) / 2)};
        level = createImage(ctx_->getDevice(), ctx_->getAllocator(), extent.width, extent.height, kLight,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    if (!frame.source.image || !frame.source.imageView ||
        std::any_of(frame.levels.begin(), frame.levels.end(), [](const auto& l) { return !l.image || !l.imageView; })) {
        LOG_WARNING("Bloom: target allocation failed - glow disabled");
        retireTargets(frame);
        return false;
    }
    // Only this slot's descriptors change, after beginFrame waited its fence.
    std::array<VkDescriptorImageInfo, kLevels> inputs{}, outputs{}, composite{};
    std::array<VkWriteDescriptorSet, kLevels * 2 + 1> writes{};
    for (uint32_t level = 0; level < kLevels; ++level) {
        inputs[level] = {sampler_, level ? frame.levels[level - 1].imageView : frame.source.imageView,
                         level ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        outputs[level] = {VK_NULL_HANDLE, frame.levels[level].imageView, VK_IMAGE_LAYOUT_GENERAL};
        composite[level] = {sampler_, frame.levels[level].imageView, VK_IMAGE_LAYOUT_GENERAL};
        writes[level * 2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.downsample[level],
            .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &inputs[level]};
        writes[level * 2 + 1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.downsample[level],
            .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &outputs[level]};
    }
    writes.back() = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.composite, .dstBinding = 0,
        .descriptorCount = kLevels, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = composite.data()};
    vkUpdateDescriptorSets(ctx_->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    LOG_INFO("Bloom: allocated ", frame.source.extent.width, "x", frame.source.extent.height, " scene copy and five glow levels");
    return true;
}

bool Bloom::record(VkCommandBuffer cmd, uint32_t slot, VkImage source, VkExtent2D extent, float strength) {
    ZoneScopedN("Bloom::record");
    if (slot >= kFrames) return false;
    auto& frame = frames_[slot];
    frame.ready = false;
    if (!usable_ || !cmd || !source || strength <= 0 || extent.width < 2 || extent.height < 2) return false;
    if (!ensureTargets(frame, extent) || !ensureComposite()) {
        usable_ = false; // failure logged once, no allocation/pipeline retries each frame
        return false;
    }
    barrier(cmd, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cmd, frame.source.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = blit.dstOffsets[1] = {static_cast<int>(extent.width), static_cast<int>(extent.height), 1};
    vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.source.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    barrier(cmd, frame.source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsample_);
    for (uint32_t level = 0; level < kLevels; ++level) {
        const auto& target = frame.levels[level];
        barrier(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsampleLayout_, 0, 1,
                                &frame.downsample[level], 0, nullptr);
        const int first = level == 0;
        vkCmdPushConstants(cmd, downsampleLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(first), &first);
        vkCmdDispatch(cmd, (target.extent.width + 7) / 8, (target.extent.height + 7) / 8, 1);
        barrier(cmd, target.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
    frame.strength = std::clamp(strength, 0.0f, 2.0f);
    frame.ready = true;
    return true;
}

void Bloom::composite(VkCommandBuffer cmd, uint32_t slot) {
    if (slot >= kFrames || !frames_[slot].ready || !composite_) return;
    const auto& frame = frames_[slot];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositeLayout_, 0, 1, &frame.composite, 0, nullptr);
    vkCmdPushConstants(cmd, compositeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(frame.strength), &frame.strength);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Bloom::shutdown() {
    if (!ctx_) return;
    ctx_->waitIdle();
    for (auto& frame : frames_) { retireTargets(frame); frame = {}; }
    const auto device = ctx_->getDevice();
    destroy(device, pool_);
    destroyPipeline(device, downsample_, downsampleLayout_);
    destroyPipeline(device, composite_, compositeLayout_);
    destroy(device, downsampleSetLayout_);
    destroy(device, compositeSetLayout_);
    sampler_ = VK_NULL_HANDLE;
    usable_ = false;
    ctx_ = nullptr;
}
}
