#include "rendering/frame_targets.hpp"

#include <sstream>

namespace wowee {
namespace rendering {

const char* vkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    default: return "VkResult";
    }
}

void FrameTargets::rebuilding() { usable_ = false; }

std::optional<std::string> FrameTargets::failed(std::string_view target, VkResult result, uint32_t width,
                                                uint32_t height, const std::optional<GpuHeapReading>& heap) {
    usable_ = false;
    ++failures_;
    if (failing_) return std::nullopt;
    failing_ = true;
    std::ostringstream line;
    line << "Frame targets: the " << target << " (" << width << "x" << height << ") could not be made: "
         << vkResultName(result) << " (" << static_cast<int>(result) << ")";
    if (heap) {
        line << "; video memory " << heap->usageBytes / (1024 * 1024) << " MB in use of a "
             << heap->budgetBytes / (1024 * 1024) << " MB budget";
    }
    line << ". Frames are skipped and the targets rebuilt each frame until they can be; said again "
            "only once they have been.";
    return line.str();
}

std::optional<std::string> FrameTargets::made() {
    usable_ = true;
    if (!failing_) return std::nullopt;
    failing_ = false;
    std::ostringstream line;
    line << "Frame targets: made again after " << failures_ << " failed attempt(s); frames are drawn";
    failures_ = 0;
    return line.str();
}

} // namespace rendering
} // namespace wowee
