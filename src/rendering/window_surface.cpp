#include "rendering/window_surface.hpp"

#include "foundation/logger.hpp"
#include "platform/config_paths.hpp"
#include "rendering/vk_context.hpp"

namespace wowee::rendering {

WindowSurface::WindowSurface(core::Window& window) : window_(window) {}

WindowSurface::~WindowSurface() {
    if (context_) {
        window_.setDisplayObserver(nullptr);
        context_->shutdown();
    }
}

bool WindowSurface::initialize() {
    context_ = std::make_unique<VkContext>();
    context_->setVsync(window_.isVsyncEnabled());
    if (!context_->initialize(window_.getSDLWindow())) {
        LOG_ERROR("Failed to initialize Vulkan context for the window");
        return false;
    }
#ifdef __ANDROID__
    core::enterResourceRoot();
#endif
    window_.setDisplayObserver(this);
    return true;
}

void WindowSurface::windowDisplayChanged() {
    context_->setVsync(window_.isVsyncEnabled());
    context_->markSwapchainDirty();
}

} // namespace wowee::rendering
