#pragma once

// A GPU cull's verdict reaches the CPU a frame or two after the frame it
// culled, and the camera may have turned and moved since. So the cull tests
// against a guard - that frame's view widened by an angle and its side
// planes pushed out by a distance, its ranges stretched by the same - and a
// verdict is used only while its guard still covers the camera's view: then
// whatever is in view now was inside the guard then, and is in the verdict.
// When it does not - a flick of the mouse, a teleport - the caller culls on
// the CPU for that frame, as it does before any verdict has come back.

#include "rendering/camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace wowee::rendering {

/// How far the camera may turn, in degrees beyond its view's edges, and move,
/// in yards, before a verdict no longer covers it.
inline constexpr float kCullGuardAngleDegrees = 15.0f;
inline constexpr float kCullGuardDistance = 10.0f;
/// How much the ranges a verdict was culled with may have grown since; the
/// cull stretches them by as much.
inline constexpr float kCullGuardRangeSlack = 1.1f;

struct CullGuard {
    /// Where the camera stood.
    glm::vec3 position{0.0f};
    /// The four side planes, inward: xyz the unit normal, w the distance,
    /// pushed out by kCullGuardDistance. A point p is inside when
    /// dot(xyz, p) + w >= 0.
    std::array<glm::vec4, 4> planes{};
    /// False for the guard of no view, which covers nothing.
    bool valid = false;
};

/// The guard of `camera`'s view.
inline CullGuard cullGuardFor(const Camera& camera) {
    const glm::vec3 f = camera.getForward();
    const glm::vec3 r = camera.getRight();
    const glm::vec3 u = camera.getUp();
    const float tanV = std::tan(glm::radians(camera.getFovDegrees()) * 0.5f);
    const float tanH = tanV * camera.getAspectRatio();
    const float widen = glm::radians(kCullGuardAngleDegrees);
    const float aH = std::min(std::atan(tanH) + widen, glm::half_pi<float>());
    const float aV = std::min(std::atan(tanV) + widen, glm::half_pi<float>());
    // Each side plane through the eye, turned out from the view's edge.
    const glm::vec3 normals[4] = {std::sin(aH) * f + std::cos(aH) * r, std::sin(aH) * f - std::cos(aH) * r,
                                  std::sin(aV) * f + std::cos(aV) * u, std::sin(aV) * f - std::cos(aV) * u};
    CullGuard guard;
    guard.position = camera.getPosition();
    for (size_t i = 0; i < 4; ++i) {
        const glm::vec3 n = glm::normalize(normals[i]);
        guard.planes[i] = glm::vec4(n, -glm::dot(n, guard.position) + kCullGuardDistance);
    }
    guard.valid = true;
    return guard;
}

/// Whether everything in `camera`'s view now lies inside `guard`: the camera
/// moved no further than the planes were pushed out, and every edge of its
/// view points inside the guard's widened sides. A point in view now, minus
/// the move, is then inside the guard's sides as they stood at its eye.
inline bool cullGuardCovers(const CullGuard& guard, const Camera& camera) {
    if (!guard.valid) return false;
    if (glm::length(camera.getPosition() - guard.position) > kCullGuardDistance) return false;
    const glm::vec3 f = camera.getForward();
    const glm::vec3 r = camera.getRight();
    const glm::vec3 u = camera.getUp();
    const float tanV = std::tan(glm::radians(camera.getFovDegrees()) * 0.5f);
    const float tanH = tanV * camera.getAspectRatio();
    for (const float sx : {-1.0f, 1.0f}) {
        for (const float sy : {-1.0f, 1.0f}) {
            const glm::vec3 corner = f + sx * tanH * r + sy * tanV * u;
            for (const auto& plane : guard.planes) {
                if (glm::dot(glm::vec3(plane), corner) < 0.0f) return false;
            }
        }
    }
    return true;
}

/// WOWEE_CULL_ON_CPU=1: every view culled on the CPU, no GPU verdict used -
/// the picture a verdict should give, to compare it with.
inline bool cullOnCpu() {
    static const bool onCpu = [] {
        const char* set = std::getenv("WOWEE_CULL_ON_CPU");
        return set && *set && *set != '0';
    }();
    return onCpu;
}

/// Whether a range now is one a verdict culled with `then` still covers: the
/// cull tests against then * kCullGuardRangeSlack + kCullGuardDistance.
inline bool cullGuardRangeCovers(float then, float now) { return now <= then * kCullGuardRangeSlack; }

}  // namespace wowee::rendering
