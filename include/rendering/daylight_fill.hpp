#pragma once

#include <glm/glm.hpp>

namespace wowee::rendering {

// Outdoor daylight includes scattered light from the sky. Transfer a small
// share of the authored direct light into that fill, preserving ambient +
// diffuse in every channel so sun-facing surfaces cannot become overexposed.
//
// At every hour: the client's sun never goes below the horizon (see
// sunLightDirection), so there is no sunrise to fade this in at. Night keeps
// its curve through the bands themselves, whose direct light is small then.
inline void applyDaylightFill(glm::vec3& ambient, glm::vec3& diffuse) {
    const glm::vec3 fill = diffuse * 0.20f;
    ambient += fill;
    diffuse -= fill;
}

} // namespace wowee::rendering
