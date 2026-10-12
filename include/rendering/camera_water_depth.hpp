#pragma once

/// How far the camera is under the water above it, or nothing when that water
/// is not the water the camera is in.
///
/// The water query looks 400 units up so deep ocean still counts, and it only
/// checks the surface's footprint and height. A dry cave beneath a lake sits
/// inside that footprint and below that surface, so it came out as under
/// water: underwater light and blue fog in the cave. The ground tells them
/// apart. Water stands on the ground, so a camera below solid ground at its own
/// position is in a cave under it, however deep. Over a terrain hole the
/// ground says nothing, and the water decides.

#include <optional>

namespace wowee::rendering {

/// How far the camera can sit below the ground and still be in the water
/// over it: the near plane clips into a lake bed when the camera hugs it.
inline constexpr float CAMERA_BELOW_GROUND_TOLERANCE = 0.5f;

/// `waterHeight` is the surface the water query found, `groundHeight` the
/// terrain under the camera when there is a tile there, and `groundIsHole`
/// whether that terrain quad is cut away.
inline std::optional<float> cameraWaterDepth(float cameraZ, std::optional<float> waterHeight,
                                             std::optional<float> groundHeight,
                                             bool groundIsHole) {
    if (!waterHeight) return std::nullopt;
    if (groundHeight && !groundIsHole &&
        cameraZ < *groundHeight - CAMERA_BELOW_GROUND_TOLERANCE) {
        return std::nullopt;
    }
    return *waterHeight - cameraZ;
}

}  // namespace wowee::rendering
