#include "rendering/sky_background.hpp"
#include "rendering/vk_context.hpp"
#include "core/logger.hpp"
#include <algorithm>

namespace wowee::rendering {
bool SkyBackground::prepare(VkContext& ctx, uint32_t slot, VkExtent2D sceneExtent) {
    ctx_ = &ctx;
    if (slot >= frames_.size() || !sceneExtent.width || !sceneExtent.height) return false;
    auto& f = frames_[slot];
    const uint32_t width = 256;
    const uint32_t height = std::max(1u, width * sceneExtent.height / sceneExtent.width);
    const auto samples = ctx.getMsaaSamples();
    const bool depthResolve = samples != VK_SAMPLE_COUNT_1_BIT && ctx.getDepthResolveImageView();
    if (f.framebuffer && f.color.extent.width == width && f.color.extent.height == height &&
        f.color.format == ctx.getSwapchainFormat() && f.samples == samples &&
        f.compatiblePass == ctx.getImGuiRenderPass() &&
        bool(f.depthResolve.image) == depthResolve) return true;
    destroy(f);
    const auto device = ctx.getDevice();
    const auto alloc = ctx.getAllocator();
    f.samples = samples;
    f.compatiblePass = ctx.getImGuiRenderPass();
    f.color = createImage(device, alloc, width, height, ctx.getSwapchainFormat(),
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    f.depth = createImage(device, alloc, width, height, ctx.getDepthFormat(),
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, samples);
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        f.msaaColor = createImage(device, alloc, width, height, ctx.getSwapchainFormat(),
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, samples);
    }
    if (depthResolve) {
        f.depthResolve = createImage(device, alloc, width, height, ctx.getDepthFormat(),
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }
    VkImageView attachments[4]{f.color.imageView, f.depth.imageView};
    uint32_t count = 2;
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        attachments[0] = f.msaaColor.imageView;
        attachments[2] = f.color.imageView;
        count = 3;
        if (depthResolve) attachments[count++] = f.depthResolve.imageView;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!attachments[i]) { destroy(f); return false; }
    }
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = f.compatiblePass;
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.width = width;
    info.height = height;
    info.layers = 1;
    if (vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        LOG_ERROR("Sky background framebuffer creation failed");
        destroy(f);
        return false;
    }
    return true;
}
void SkyBackground::begin(VkCommandBuffer cmd, uint32_t slot) {
    auto& f = frames_[slot];
    VkClearValue clears[4]{};
    clears[0].color = {{0, 0, 0, 1}};
    clears[1].depthStencil = {1, 0};
    clears[3].depthStencil = {1, 0};
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = f.compatiblePass;
    info.framebuffer = f.framebuffer;
    info.renderArea.extent = f.color.extent;
    info.clearValueCount = f.samples == VK_SAMPLE_COUNT_1_BIT ? 2 : (f.depthResolve.image ? 4 : 3);
    info.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.width = float(f.color.extent.width);
    viewport.height = float(f.color.extent.height);
    viewport.maxDepth = 1;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, f.color.extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void SkyBackground::end(VkCommandBuffer cmd, uint32_t slot) {
    vkCmdEndRenderPass(cmd);
    // The compatible scene pass finishes its resolved color in PRESENT layout,
    // including offscreen targets. Make this slot readable before world draws.
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = frames_[slot].color.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    cmdPipelineBarrier2(cmd, dep);
}
void SkyBackground::destroy(Frame& f) {
    if (!ctx_) return;
    const auto device = ctx_->getDevice();
    if (f.framebuffer) vkDestroyFramebuffer(device, f.framebuffer, nullptr);
    destroyImage(device, ctx_->getAllocator(), f.color);
    destroyImage(device, ctx_->getAllocator(), f.depth);
    destroyImage(device, ctx_->getAllocator(), f.msaaColor);
    destroyImage(device, ctx_->getAllocator(), f.depthResolve);
    f = {};
}
void SkyBackground::shutdown() {
    for (auto& f : frames_) destroy(f);
    ctx_ = nullptr;
}
}
