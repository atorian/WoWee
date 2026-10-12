#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <optional>

namespace wowee::rendering {

/// The light a model standing in a WMO interior group takes instead of the
/// frame's: an ambient, and a directional light falling from above (its
/// direction is in interior_light.glsli). The sun does not reach inside.
struct InteriorLight {
    glm::vec3 ambient{0.0f};
    glm::vec3 direct{0.0f};
};

/// InteriorLight as the shaders read it (interior_light.glsli): ambient.w is
/// 1 inside an interior group and 0 outdoors, where the frame's light is used.
struct InteriorLightGPU {
    glm::vec4 ambient{0.0f};
    glm::vec4 direct{0.0f};
};

[[nodiscard]] inline InteriorLightGPU packInteriorLight(const std::optional<InteriorLight>& light) {
    if (!light) return {};
    return {.ambient = glm::vec4(light->ambient, 1.0f), .direct = glm::vec4(light->direct, 0.0f)};
}

/// A doodad an interior group lists: the building's MOHD ambient, and the
/// colour its MODD entry carries as its direct light.
[[nodiscard]] inline InteriorLight doodadInteriorLight(const glm::vec3& wmoAmbient,
                                                       const glm::vec3& doodadColor) {
    return {.ambient = wmoAmbient, .direct = doodadColor};
}

/// A character or creature inside: lit by the floor it stands on. The floor
/// draws with MOHD + 2 * MOCV (wmo.frag), and that colour is split the way
/// the client splits it (AdjustLighting in WebWowViewerCpp): the direct light
/// is brought up to a brightness of 0xA8 and the ambient held down to 0x60,
/// so a unit in a dim room is still lit from above. With no vertex colour
/// under it the MOHD ambient alone is split.
[[nodiscard]] inline InteriorLight unitInteriorLight(const glm::vec3& wmoAmbient,
                                                     const std::optional<glm::vec3>& floorColor) {
    constexpr float kMinDirect = 0xA8 / 255.0f;
    constexpr float kMaxAmbient = 0x60 / 255.0f;

    const glm::vec3 floor =
        glm::min(wmoAmbient + 2.0f * floorColor.value_or(glm::vec3(0.0f)), glm::vec3(1.0f));
    const float brightest = std::max({floor.r, floor.g, floor.b});
    if (brightest <= 0.0f) return {};

    return {
        .ambient = brightest > kMaxAmbient ? floor * (kMaxAmbient / brightest) : floor,
        .direct = brightest < kMinDirect ? floor * (kMinDirect / brightest) : floor,
    };
}

/// The vertex colour of a triangle at a point on it, interpolated between
/// its corners.
[[nodiscard]] inline glm::vec3 colorAtPoint(const glm::vec3& p, const glm::vec3& v0,
                                            const glm::vec3& v1, const glm::vec3& v2,
                                            const glm::vec3& c0, const glm::vec3& c1,
                                            const glm::vec3& c2) {
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 d = p - v0;
    const float d11 = glm::dot(e1, e1);
    const float d12 = glm::dot(e1, e2);
    const float d22 = glm::dot(e2, e2);
    const float denom = d11 * d22 - d12 * d12;
    if (denom == 0.0f) return c0;  // degenerate: no inside to interpolate across
    const float w1 = (d22 * glm::dot(d, e1) - d12 * glm::dot(d, e2)) / denom;
    const float w2 = (d11 * glm::dot(d, e2) - d12 * glm::dot(d, e1)) / denom;
    return c0 * (1.0f - w1 - w2) + c1 * w1 + c2 * w2;
}

}  // namespace wowee::rendering
