#pragma once

#include <glm/glm.hpp>

namespace wowee::rendering {

// Outdoor daylight includes scattered light from the sky. Transfer a small
// share of the authored direct light into that fill, preserving ambient +
// diffuse in every channel so sun-facing surfaces cannot become overexposed.
// Fade out at the horizon: night and permanently dark zones keep their curves.
inline void applyDaylightFill(glm::vec3& ambient, glm::vec3& diffuse,
                             const glm::vec3& lightTravelDir) {
    const float daylight = glm::smoothstep(0.0f, 0.2f, -lightTravelDir.z);
    const glm::vec3 fill = diffuse * (0.20f * daylight);
    ambient += fill;
    diffuse -= fill;
}

} // namespace wowee::rendering
