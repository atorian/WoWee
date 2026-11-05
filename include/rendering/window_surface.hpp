#pragma once

#include "platform/window.hpp"
#include "rendering/render_surface.hpp"

#include <memory>

namespace wowee::rendering {

/// The Vulkan presentation of a platform window. The composition root owns
/// both, destroys the renderer first, this surface next, and the window last.
class WindowSurface final : public RenderSurface, private core::WindowDisplayObserver {
public:
    explicit WindowSurface(core::Window& window);
    ~WindowSurface() override;
    WindowSurface(const WindowSurface&) = delete;
    WindowSurface& operator=(const WindowSurface&) = delete;

    [[nodiscard]] bool initialize();
    [[nodiscard]] VkContext* getVkContext() const override { return context_.get(); }
    [[nodiscard]] int getWidth() const override { return window_.getWidth(); }
    [[nodiscard]] int getHeight() const override { return window_.getHeight(); }
    [[nodiscard]] int getDrawableWidth() const override { return window_.getDrawableWidth(); }
    [[nodiscard]] int getDrawableHeight() const override { return window_.getDrawableHeight(); }

private:
    void windowDisplayChanged() override;
    core::Window& window_;
    std::unique_ptr<VkContext> context_;
};

} // namespace wowee::rendering
