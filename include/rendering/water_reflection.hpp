#pragma once

#include "rendering/frustum.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <optional>
#include <span>

namespace wowee {
namespace rendering {

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

/// A water surface in the camera's view, as the reflection is chosen from:
/// the box its waves can carry it through, the height it lies at, and
/// whether any of it reached the screen (WaterVisibility) - in the view and
/// hidden behind something, it shows no mirror.
struct WaterInView {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    float height = 0.0f;
    bool seen = true;
};

/// The camera's view, as the reflection is made from it.
struct ReflectedCamera {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 1.0f, 0.0f};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
};

/// The water's planar reflection for one frame: one plane, the part of the
/// mirrored image the water on it shows, and what can be seen there.
///
/// The image is drawn for the water on one plane only. Water lying higher
/// or lower than that reflects the sky alone (mirrors()), because the
/// mirror of another plane shows the wrong place on it.
struct WaterReflectionView {
    /// The plane, z up.
    float height = 0.0f;
    /// The mirrored camera: mirroredView and obliqueReflectionProjection.
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    /// Where in the mirrored image the water on the plane can show it, as
    /// x0, y0, x1, y1 from 0 to 1 across the image (y down, as the image is
    /// stored). Nothing outside it is ever read.
    glm::vec4 area{0.0f, 0.0f, 1.0f, 1.0f};

    /// Whether anything inside the box can show in the water: above the
    /// plane, and seen by the mirrored camera through `area`.
    [[nodiscard]] bool canSee(const glm::vec3& boxMin, const glm::vec3& boxMax) const;
    /// The same for a sphere.
    [[nodiscard]] bool canSee(const glm::vec3& centre, float radius) const;
    /// Whether `water` lies on the plane and reflects the image.
    [[nodiscard]] bool mirrors(const WaterInView& water) const;

    /// The mirrored camera narrowed to `area`, its near plane the camera's.
    Frustum seen;
};

/// The reflection for a camera over `water`, the surfaces in its view: on the
/// plane whose water covers most of the picture, among the planes below the
/// camera, of the water that reaches the screen. None when no such water
/// lies below it.
[[nodiscard]] std::optional<WaterReflectionView> waterReflectionView(const ReflectedCamera& camera,
                                                                     std::span<const WaterInView> water);

} // namespace rendering
} // namespace wowee
