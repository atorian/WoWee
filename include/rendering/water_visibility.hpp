#pragma once

#include "rendering/liquid_scene.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace wowee::rendering {

/// Which water surfaces reached the screen, from the GPU's occlusion answers
/// for the frames that drew them.
///
/// A surface can stand in the camera's view and still show nowhere: Dalaran's
/// canals run under its streets. The water's reflection is chosen from the
/// surfaces seen, so the city is not drawn again into a mirror nobody looks
/// at. The answers come back on the render thread a frame or two after the
/// draw, and are read on the main thread when the next frame's scene is
/// gathered.
class WaterVisibility {
public:
    /// What the GPU answered for `surface`: how many of its samples passed
    /// the depth test in the frame that drew it.
    void answer(LiquidSurfaceId surface, uint64_t samples) {
        std::lock_guard lock(mutex_);
        seen_[surface] = samples > 0;
    }
    /// Whether the surface reached the screen the last time it was asked
    /// about. One never asked about counts as seen, so water that comes into
    /// view reflects from its first frame.
    [[nodiscard]] bool seen(LiquidSurfaceId surface) const {
        std::lock_guard lock(mutex_);
        const auto found = seen_.find(surface);
        return found == seen_.end() || found->second;
    }
    /// The surface is gone; an id given out again is new water.
    void forget(LiquidSurfaceId surface) {
        std::lock_guard lock(mutex_);
        seen_.erase(surface);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<LiquidSurfaceId, bool> seen_;
};

}  // namespace wowee::rendering
