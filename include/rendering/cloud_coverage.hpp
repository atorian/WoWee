#pragma once

/// The cloud layer's coverage in a direction, as clouds.frag draws it: the
/// shader's own include compiled as C++, so the sun's glare is dimmed by the
/// cloud actually drawn in front of the sun (WoW.exe 0x7ef920, 0x7efa30).

#include <glm/glm.hpp>

namespace wowee::rendering::cloud_coverage {
using glm::clamp;
using glm::dot;
using glm::floor;
using glm::fract;
using glm::mix;
using glm::sin;
using glm::smoothstep;
using mat2 = glm::mat2;
using vec2 = glm::vec2;
using vec3 = glm::vec3;
#include "../../assets/shaders/cloud_coverage.glsli"

/// Coverage, 0-1, toward `dir` (render space, unit); none at and below the
/// horizon, where the layer is not drawn.
inline float coverageToward(const glm::vec3& dir, float density, float windOffset) {
    if (dir.z <= 0.0f) return 0.0f;
    return cloudShapeAt(dir, density, windOffset).cloud;
}

}  // namespace wowee::rendering::cloud_coverage
