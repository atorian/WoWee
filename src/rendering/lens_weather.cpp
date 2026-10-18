#include "rendering/lens_weather.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

constexpr VkFormat kCopyFormat = VK_FORMAT_R8G8B8A8_UNORM;

/// Must match Drop in lens_drops.vert.glsl.
struct GpuDrop {
    glm::vec4 posRadius;
    glm::vec4 params;
};

/// Must match Push in lens_film.frag.glsl and lens_drops.*.glsl.
struct LensPush {
    glm::vec4 film;
    glm::vec4 wind;
};

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  uint32_t baseMip = 0, uint32_t mipCount = 1) {
    VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = baseMip,
                          .levelCount = mipCount, .baseArrayLayer = 0, .layerCount = 1};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

}  // namespace

LensWeather::~LensWeather() {
    shutdown();
}

bool LensWeather::initialize(VkContext* ctx) {
    if (!ctx) return false;
    ctx_ = ctx;
    VkDevice device = ctx_->getDevice();

    VkFormatProperties swapProps{};
    VkFormatProperties copyProps{};
    vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), ctx_->getSwapchainFormat(), &swapProps);
    vkGetPhysicalDeviceFormatProperties(ctx_->getPhysicalDevice(), kCopyFormat, &copyProps);
    if (!(swapProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(copyProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        LOG_WARNING("LensWeather: the swapchain cannot be copied down here - no rain, snow or sand on the lens");
        return false;
    }

    VkSamplerCreateInfo sampCI{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    // Between the copy's levels too: a drop is out of focus by how far off
    // the lens is from what it looks at, not in steps.
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampCI.maxLod = VK_LOD_CLAMP_NONE;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ = ctx_->getOrCreateSampler(sampCI);

    const VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT},
    };
    setLayout_ = createDescriptorSetLayout(device, {bindings[0], bindings[1]});
    const VkPushConstantRange push{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   .offset = 0, .size = sizeof(LensPush)};
    layout_ = createPipelineLayout(device, {setLayout_}, {push});
    if (sampler_ == VK_NULL_HANDLE || setLayout_ == VK_NULL_HANDLE || layout_ == VK_NULL_HANDLE) {
        LOG_ERROR("LensWeather: could not create its layouts - no rain, snow or sand on the lens");
        return false;
    }

    const VkDescriptorPoolSize sizes[2] = {
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES_IN_FLIGHT},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MAX_FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &pool_) != VK_SUCCESS) {
        LOG_ERROR("LensWeather: could not create its descriptor pool - no rain, snow or sand on the lens");
        return false;
    }
    for (Slot& slot : slots_) {
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &setLayout_;
        slot.drops = createBuffer(ctx_->getAllocator(), sizeof(GpuDrop) * LensDrops::kMaxDrops,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        if (vkAllocateDescriptorSets(device, &alloc, &slot.set) != VK_SUCCESS || !slot.drops.info.pMappedData) {
            LOG_ERROR("LensWeather: could not allocate its sets and buffers - no rain, snow or sand on the lens");
            return false;
        }
    }

    // Now rather than at the first rainy frame, so a pipeline that will not
    // build says so at start-up.
    if (!ensurePipelines()) return false;
    usable_ = true;
    return true;
}

void LensWeather::shutdown() {
    if (!ctx_) return;
    VkDevice device = ctx_->getDevice();
    vkDeviceWaitIdle(device);
    for (Slot& slot : slots_) {
        destroyImage(device, ctx_->getAllocator(), slot.copy);
        destroyBuffer(ctx_->getAllocator(), slot.drops);
        slot.set = VK_NULL_HANDLE;
        slot.draw = false;
    }
    destroy(device, filmPipeline_);
    destroy(device, dropsPipeline_);
    destroy(device, layout_);
    destroy(device, pool_);
    destroy(device, setLayout_);
    sampler_ = VK_NULL_HANDLE;
    copyExtent_ = sourceExtent_ = {.width = 0, .height = 0};
    usable_ = false;
    ctx_ = nullptr;
}

bool LensWeather::ensurePipelines() {
    const VkFormat format = ctx_->getSwapchainFormat();
    if (filmPipeline_ != VK_NULL_HANDLE && dropsPipeline_ != VK_NULL_HANDLE && format == pipelineFormat_) {
        return true;
    }
    VkDevice device = ctx_->getDevice();
    // A rebuild replaces pipelines an in-flight frame may have bound.
    DeferredResources retired(*ctx_);
    retired.destroy(filmPipeline_);
    retired.destroy(dropsPipeline_);

    const auto build = [&](const char* vert, const char* frag, const char* name) -> VkPipeline {
        ShaderPair shaders = loadShaderPair(device, vert, frag, name);
        if (!shaders) return VK_NULL_HANDLE;
        return PipelineBuilder()
            .setShaders(shaders.vertStage, shaders.fragStage)
            .setVertexInput({}, {})
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setMultisample(VK_SAMPLE_COUNT_1_BIT)
            .setLayout(layout_)
            .setRenderPass(ctx_->getOverlayRenderPass())
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .build(device, ctx_->getPipelineCache());
    };
    filmPipeline_ = build("assets/shaders/postprocess.vert.spv", "assets/shaders/lens_film.frag.spv", "lens film");
    dropsPipeline_ = build("assets/shaders/lens_drops.vert.spv", "assets/shaders/lens_drops.frag.spv",
                           "lens drops");
    if (filmPipeline_ == VK_NULL_HANDLE || dropsPipeline_ == VK_NULL_HANDLE) {
        LOG_ERROR("LensWeather: could not build its pipelines - no rain, snow or sand on the lens");
        return false;
    }
    pipelineFormat_ = format;
    return true;
}

void LensWeather::destroyCopies() {
    if (!ctx_) return;
    DeferredResources retired(*ctx_);
    for (Slot& slot : slots_) {
        retired.image(slot.copy);
        slot.draw = false;
    }
    copyExtent_ = sourceExtent_ = {.width = 0, .height = 0};
}

bool LensWeather::ensureCopies(VkExtent2D extent) {
    if (extent.width == sourceExtent_.width && extent.height == sourceExtent_.height && slots_[0].copy.image) {
        return true;
    }
    destroyCopies();
    // Half a side, and its levels below it: what is on the lens is out of
    // focus, and it is the levels that blur what it shows.
    copyExtent_ = {.width = std::max(1u, extent.width / 2), .height = std::max(1u, extent.height / 2)};
    copyMips_ = 1;
    while (copyMips_ < kCopyMips && (copyExtent_.width >> copyMips_) >= 4 && (copyExtent_.height >> copyMips_) >= 4) {
        ++copyMips_;
    }
    for (Slot& slot : slots_) {
        slot.copy = createImage(ctx_->getDevice(), ctx_->getAllocator(), copyExtent_.width, copyExtent_.height,
                                kCopyFormat,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_SAMPLE_COUNT_1_BIT, copyMips_);
        if (!slot.copy.image) {
            LOG_WARNING("LensWeather: could not allocate ", copyExtent_.width, "x", copyExtent_.height,
                        " copies of the frame - nothing drawn on the lens");
            destroyCopies();
            usable_ = false;
            return false;
        }
    }
    sourceExtent_ = extent;
    return true;
}

void LensWeather::writeSet(uint32_t frame) {
    Slot& slot = slots_[frame];
    const VkDescriptorImageInfo image{.sampler = sampler_, .imageView = slot.copy.imageView,
                                      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo buffer{.buffer = slot.drops.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]{};
    for (auto& write : w) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = slot.set;
        write.descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &image;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &buffer;
    vkUpdateDescriptorSets(ctx_->getDevice(), 2, w, 0, nullptr);
}

bool LensWeather::record(VkCommandBuffer cmd, uint32_t frame, VkImage swapchainImage, VkExtent2D extent,
                         const LensDrops& lens, glm::vec2 windOnScreen, float time) {
    ZoneScopedN("LensWeather::record");
    if (frame >= slots_.size()) return false;
    Slot& slot = slots_[frame];
    slot.draw = false;
    if (!usable_ || lens.empty() || cmd == VK_NULL_HANDLE || swapchainImage == VK_NULL_HANDLE ||
        extent.width < 4 || extent.height < 4) {
        return false;
    }
    if (!ensureCopies(extent) || !ensurePipelines()) return false;
    writeSet(frame);

    // What is on the lens, for the drops' vertices.
    auto* out = static_cast<GpuDrop*>(slot.drops.info.pMappedData);
    uint32_t n = 0;
    for (const LensDrop& d : lens.drops()) {
        if (n >= LensDrops::kMaxDrops) break;
        const float opacity = d.opacity();
        if (opacity <= 0.0f) continue;
        out[n++] = GpuDrop{
            .posRadius = glm::vec4(d.pos, d.radius, opacity),
            // y: how fast a drop runs; how far a flake has melted.
            .params = glm::vec4(static_cast<float>(d.kind),
                                d.kind == LensDrop::Kind::Flake ? d.age / std::max(d.life, 1e-3f)
                                                                : std::max(d.velocity.y, 0.0f),
                                d.seed, std::atan2(d.velocity.y, d.velocity.x)),
        };
    }
    slot.dropCount = n;
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    slot.filmPush = glm::vec4(lens.frost(), lens.dust(), aspect, time);
    slot.windPush = glm::vec4(windOnScreen, 0.0f, 0.0f);
    slot.film = lens.frost() > 0.001f || lens.dust() > 0.001f;
    if (slot.dropCount == 0 && !slot.film) return false;

    // The finished frame, from wherever its last pass left it, copied down.
    imageBarrier(cmd, swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    // Its last contents are not wanted; its composite read them two frames ago.
    imageBarrier(cmd, slot.copy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, copyMips_);
    VkImageBlit blit{};
    blit.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1};
    blit.dstOffsets[1] = {.x = static_cast<int32_t>(copyExtent_.width),
                          .y = static_cast<int32_t>(copyExtent_.height), .z = 1};
    vkCmdBlitImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.copy.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    // Back for the overlay pass, which loads it from PRESENT_SRC.
    imageBarrier(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // Each level from the one above it.
    for (uint32_t level = 1; level < copyMips_; ++level) {
        imageBarrier(cmd, slot.copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
        VkImageBlit down{};
        down.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level - 1, .baseArrayLayer = 0,
                               .layerCount = 1};
        down.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level, .baseArrayLayer = 0,
                               .layerCount = 1};
        down.srcOffsets[1] = {.x = std::max(1, static_cast<int32_t>(copyExtent_.width >> (level - 1))),
                              .y = std::max(1, static_cast<int32_t>(copyExtent_.height >> (level - 1))), .z = 1};
        down.dstOffsets[1] = {.x = std::max(1, static_cast<int32_t>(copyExtent_.width >> level)),
                              .y = std::max(1, static_cast<int32_t>(copyExtent_.height >> level)), .z = 1};
        vkCmdBlitImage(cmd, slot.copy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.copy.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down, VK_FILTER_LINEAR);
    }
    if (copyMips_ > 1) {
        imageBarrier(cmd, slot.copy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, 0, copyMips_ - 1);
    }
    imageBarrier(cmd, slot.copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, copyMips_ - 1, 1);

    slot.draw = true;
    return true;
}

void LensWeather::composite(VkCommandBuffer cmd, uint32_t frame) {
    if (frame >= slots_.size()) return;
    const Slot& slot = slots_[frame];
    if (!slot.draw) return;
    const LensPush push{.film = slot.filmPush, .wind = slot.windPush};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &slot.set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    if (slot.film) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filmPipeline_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    if (slot.dropCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dropsPipeline_);
        vkCmdDraw(cmd, 6, slot.dropCount, 0, 0);
    }
}

}  // namespace rendering
}  // namespace wowee
