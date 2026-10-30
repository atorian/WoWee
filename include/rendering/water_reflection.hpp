#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace wowee {
namespace rendering {

class Camera;

/// The view of a camera at `cameraPos` looking along `forward`, mirrored in
/// the water's plane at height `waterHeight` (z up): from below the plane,
/// looking up at what stands above it.
[[nodiscard]] glm::mat4 mirroredView(glm::vec3 cameraPos, glm::vec3 forward, float waterHeight);

/// `projection` with its near plane laid on the water's plane, so the
/// mirrored view (`view`) draws only what stands above the water and
/// nothing of the bed under it (Lengyel's oblique near plane, for the
/// zero-to-one depth this renderer uses).
[[nodiscard]] glm::mat4 obliqueReflectionProjection(const glm::mat4& projection,
                                                    const glm::mat4& view, float waterHeight);

/// The eye the water's reflection is drawn from: the camera mirrored in the
/// water at `waterHeight`, with the near plane on the water.
struct WaterReflectionView {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
};

/// The reflection's eye for `camera`, or none where no reflection is drawn:
/// no water to reflect in, or a camera less than half a yard above it.
[[nodiscard]] std::optional<WaterReflectionView> waterReflectionView(const Camera& camera,
                                                                     std::optional<float> waterHeight);

} // namespace rendering
} // namespace wowee
