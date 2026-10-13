#pragma once

namespace wowee {
namespace rendering {

class VkContext;

/// What the renderer draws onto: a Vulkan context with a swapchain, and the
/// size of what that swapchain presents to. The client's window is one; an
/// offline scene's hidden window is another.
class RenderSurface {
public:
    virtual ~RenderSurface() = default;

    [[nodiscard]] virtual VkContext* getVkContext() const = 0;
    /// In the units the desktop places windows in - points, which the
    /// interface is laid out in.
    [[nodiscard]] virtual int getWidth() const = 0;
    [[nodiscard]] virtual int getHeight() const = 0;
    /// In pixels: what the swapchain has to be.
    [[nodiscard]] virtual int getDrawableWidth() const = 0;
    [[nodiscard]] virtual int getDrawableHeight() const = 0;
    [[nodiscard]] float getAspectRatio() const {
        return static_cast<float>(getWidth()) / static_cast<float>(getHeight());
    }
};

} // namespace rendering
} // namespace wowee
