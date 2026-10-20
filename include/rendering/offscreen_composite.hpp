#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vulkan/vulkan.h>

namespace wowee {
namespace rendering {

/// Something drawn into its own image at the start of the frame, ahead of
/// the world - a character preview, the world map - for the interface to show.
/// Registered with the renderer by whoever owns it, and unregistered before
/// it goes away.
class OffscreenComposite : public std::enable_shared_from_this<OffscreenComposite> {
public:
    using Request = std::function<void(VkCommandBuffer, uint32_t)>;
    virtual ~OffscreenComposite() = default;
    /// Producer-side extraction. The request owns its values and resource
    /// lifetime; executing it may not read the live interface or scene.
    virtual Request extractComposite() = 0;
};

} // namespace rendering
} // namespace wowee
