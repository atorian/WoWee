#pragma once

#include "rendering/rt_bvh.hpp"
#include "rendering/rt_distance.hpp"
#include <algorithm>
#include <cmath>

namespace wowee::rendering {

inline constexpr float kRtAoRadius = 3.0f;
inline constexpr float kRtGiLength = 120.0f;
inline constexpr float kRtShadowLength = 1500.0f;
inline constexpr float kRtSunTanRadius = 0.0087f;


/// Receivers lie in a camera-centred sphere. Their indirect hits lie within
/// indirectLength of it, and shadow rays sweep that volume toward the sun.
/// A slab test against expanded bounds conservatively keeps the whole sweep,
/// including offscreen casters and the cone of soft solar shadow samples.
struct RtRegion {
    glm::vec3 centre{0};
    glm::vec3 sunDirection{0, 0, 1};
    float distance = 0; // unbounded for standalone tools
    float indirectLength = kRtGiLength;
    bool sunlight = true;
    bool enabled = true;

    bool operator==(const RtRegion&) const = default;

    bool keeps(const RtAabb& bounds, bool retained) const {
        if (!enabled || !bounds.valid()) return false;
        if (distance <= 0) return true;
        const float margin = retained ? std::max(16.0f, distance * 0.1f) : 0.0f;
        const float radius = distance + indirectLength + margin + 1.0f;
        if (!sunlight) {
            const glm::vec3 delta = centre - glm::clamp(centre, bounds.bmin, bounds.bmax);
            return glm::dot(delta, delta) <= radius * radius;
        }
        const float len = glm::length(sunDirection);
        if (len < 1e-6f) return false;
        const glm::vec3 dir = sunDirection / len;
        const glm::vec3 expand(radius + kRtShadowLength * kRtSunTanRadius);
        const glm::vec3 lo = bounds.bmin - expand, hi = bounds.bmax + expand;
        float enter = 0, exit = kRtShadowLength;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir[axis]) < 1e-6f) {
                if (centre[axis] < lo[axis] || centre[axis] > hi[axis]) return false;
            } else {
                const float a = (lo[axis] - centre[axis]) / dir[axis];
                const float b = (hi[axis] - centre[axis]) / dir[axis];
                enter = std::max(enter, std::min(a, b));
                exit = std::min(exit, std::max(a, b));
                if (enter > exit) return false;
            }
        }
        return true;
    }
};

} // namespace wowee::rendering
