#pragma once

#include "rendering/gpu_memory.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wowee {
namespace rendering {

/// The name of a VkResult as the specification spells it.
[[nodiscard]] const char* vkResultName(VkResult result);

/// Whether the frame's own targets - the depth buffer, the multisampled colour,
/// the depth resolve and the framebuffers over them - all exist, and what to
/// say when one could not be made.
///
/// A frame may be recorded only against a complete set. One missing target
/// used to leave the rest built to a size of zero and the frame recorded
/// anyway, and the driver crashed in vkCmdBeginRenderPass rather than the
/// client noticing. Now the frame is skipped and the set rebuilt next frame;
/// the failure is said once per stretch of them, with the result, the size
/// asked for and what the driver's budget had left.
class FrameTargets {
public:
    /// The set is being (re)made: nothing records until made().
    void rebuilding();
    /// One target could not be made. The line to log the first time in a
    /// stretch of failures; empty for the ones after, until the set is made.
    [[nodiscard]] std::optional<std::string> failed(std::string_view target, VkResult result,
                                                    uint32_t width, uint32_t height,
                                                    const std::optional<GpuHeapReading>& heap);
    /// Every target exists. The line saying so when it ends a stretch of
    /// failures.
    [[nodiscard]] std::optional<std::string> made();

    /// Whether a frame may be recorded against the set.
    [[nodiscard]] bool usable() const { return usable_; }

private:
    bool usable_ = false;
    bool failing_ = false;
    uint32_t failures_ = 0;
};

} // namespace rendering
} // namespace wowee
