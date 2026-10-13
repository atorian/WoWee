#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace wowee {
namespace rendering {

/// Something drawn into its own image at the start of the frame, ahead of
/// the world - a character preview, the world map - for the interface to show.
/// Registered with the renderer by whoever owns it, and unregistered before
/// it goes away.
class OffscreenComposite {
public:
    virtual ~OffscreenComposite() = default;
    /// Record its pass into the frame's command buffer, outside any render pass.
    virtual void compositeOffscreen(VkCommandBuffer cmd, uint32_t frameIndex) = 0;
};

} // namespace rendering
} // namespace wowee
