#pragma once
#include "rendering/vk_utils.hpp"
#include <array>

namespace wowee::rendering {
class VkContext;
// A small, current-frame picture of the real sky, including authored sky M2s.
// Each frame slot owns its attachments; the caller has waited on its fence.
class SkyBackground {
public:
    bool prepare(VkContext& ctx, uint32_t slot, VkExtent2D sceneExtent);
    void begin(VkCommandBuffer cmd, uint32_t slot);
    void end(VkCommandBuffer cmd, uint32_t slot);
    VkImageView view(uint32_t slot) const { return frames_[slot].color.imageView; }
    VkExtent2D extent(uint32_t slot) const { return frames_[slot].color.extent; }
    void shutdown();
private:
    struct Frame {
        AllocatedImage color, depth, msaaColor, depthResolve;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkRenderPass compatiblePass = VK_NULL_HANDLE;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    };
    void destroy(Frame& frame);
    VkContext* ctx_ = nullptr;
    std::array<Frame, 2> frames_{};
};
}
