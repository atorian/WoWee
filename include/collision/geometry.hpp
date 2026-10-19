#pragma once

/// The primitives every collision query in this client is built from, and the
/// reach that decides which instances a query looks at.
///
/// The doodad renderer and the building renderer each had their own copy of
/// these. They agreed, which is the point: a difference here does not crash or
/// log, it moves where the world is solid. A ray test that starts rejecting
/// backfaces makes floors one-sided; a distance test that is off by its own
/// radius drops the instance a character is standing on out of the query, and
/// the character falls through it.

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <glm/glm.hpp>

namespace wowee::collision {

/// Squared distance from `p` to the nearest point of the box.
///
/// Zero inside the box, which is what the callers rely on: an instance the
/// query point is inside is never further away than one it is outside.
inline float pointAABBDistanceSq(const glm::vec3& p, const glm::vec3& bmin,
                                 const glm::vec3& bmax) {
    const glm::vec3 nearest = glm::clamp(p, bmin, bmax);
    const glm::vec3 d = p - nearest;
    return glm::dot(d, d);
}

/// Moller-Trumbore. Distance along `dir` to the triangle, or negative for a
/// miss.
///
/// Two-sided on purpose. A floor is a triangle whose winding says which way
/// is up, and a character standing on the underside of a bridge or inside a
/// building still needs it to be solid, so a version that culled backfaces
/// would make half the world's surfaces one-way.
inline float rayTriangleIntersect(const glm::vec3& origin, const glm::vec3& dir,
                                  const glm::vec3& v0, const glm::vec3& v1,
                                  const glm::vec3& v2) {
    constexpr float EPSILON = 1e-6f;
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 h = glm::cross(dir, e2);
    const float a = glm::dot(e1, h);
    if (a > -EPSILON && a < EPSILON) return -1.0f;  // ray parallel to the plane

    const float f = 1.0f / a;
    const glm::vec3 s = origin - v0;
    const float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return -1.0f;

    const glm::vec3 q = glm::cross(s, e1);
    const float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return -1.0f;

    const float t = f * glm::dot(e2, q);
    return t > EPSILON ? t : -1.0f;  // behind the origin counts as a miss
}

/// Where a ray enters and leaves a box, as distances along it. False when it
/// misses. The two are what a query has to search between: for a ray that is
/// not axis-aligned in the space the box is in, the origin's XY says nothing
/// about where the ray actually crosses the box.
inline bool rayAABBRange(const glm::vec3& origin, const glm::vec3& dir,
                         const glm::vec3& bmin, const glm::vec3& bmax,
                         float& outMin, float& outMax) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(dir[i]) < 1e-8f) {
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            const float invD = 1.0f / dir[i];
            float t0 = (bmin[i] - origin[i]) * invD;
            float t1 = (bmax[i] - origin[i]) * invD;
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    if (tmax < 0.0f) return false;
    outMin = std::max(tmin, 0.0f);
    outMax = tmax;
    return true;
}

/// Whether some forward part of a ray passes through a box (slab method).
inline bool rayIntersectsAABB(const glm::vec3& origin, const glm::vec3& dir,
                              const glm::vec3& bmin, const glm::vec3& bmax) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(dir[i]) < 1e-8f) {
            // Parallel to this slab: inside it or never.
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            const float invD = 1.0f / dir[i];
            float t0 = (bmin[i] - origin[i]) * invD;
            float t1 = (bmax[i] - origin[i]) * invD;
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    return tmax >= 0.0f;
}

/// Whether the segment from `from` to `to` passes through a box, and the
/// fraction of the way along it where it enters.
inline bool segmentIntersectsAABB(const glm::vec3& from, const glm::vec3& to,
                                  const glm::vec3& bmin, const glm::vec3& bmax,
                                  float& outEnterT) {
    const glm::vec3 d = to - from;
    float tEnter = 0.0f;
    float tExit = 1.0f;
    for (int axis = 0; axis < 3; axis++) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (from[axis] < bmin[axis] || from[axis] > bmax[axis]) return false;
            continue;
        }
        const float inv = 1.0f / d[axis];
        float t0 = (bmin[axis] - from[axis]) * inv;
        float t1 = (bmax[axis] - from[axis]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) return false;
    }
    outEnterT = tEnter;
    return tExit >= 0.0f && tEnter <= 1.0f;
}

/// The world box around a model-space box placed by `transform`: the box of
/// its eight transformed corners.
inline void transformAABB(const glm::mat4& transform, const glm::vec3& localMin,
                          const glm::vec3& localMax, glm::vec3& outMin, glm::vec3& outMax) {
    const glm::vec3 corners[8] = {
        {localMin.x, localMin.y, localMin.z}, {localMin.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMin.z}, {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMin.y, localMin.z}, {localMax.x, localMin.y, localMax.z},
        {localMax.x, localMax.y, localMin.z}, {localMax.x, localMax.y, localMax.z},
    };
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    for (const glm::vec3& corner : corners) {
        const glm::vec3 world = glm::vec3(transform * glm::vec4(corner, 1.0f));
        outMin = glm::min(outMin, world);
        outMax = glm::max(outMax, world);
    }
}

/// The point of a triangle nearest to `p` (Real-Time Collision Detection,
/// 5.1.5).
inline glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a,
                                        const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    return a + ab * v + ac * w;
}

/// A per-vertex value of a triangle (a vertex colour) at a point on it,
/// interpolated between its corners.
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

/// How far from a point a query looks: everywhere, or only at instances within
/// a radius of it - which is what keeps the player's collision from walking
/// every building in the zone.
///
/// Every query is handed one. It was state each renderer kept, set by the
/// camera around the player and then read by every query, so a footprint asked
/// for a creature across town was filtered by where the player stood and found
/// no floor.
///
/// A radius of zero or less is no restriction rather than an empty sphere:
/// read the other way it would make the world non-solid rather than slow.
class Reach {
public:
    [[nodiscard]] static Reach everywhere() { return Reach{}; }
    [[nodiscard]] static Reach around(const glm::vec3& point, float radius) {
        Reach reach;
        reach.limited_ = radius > 0.0f;
        reach.point_ = point;
        reach.radius_ = std::max(0.0f, radius);
        return reach;
    }

    [[nodiscard]] bool limited() const { return limited_; }

    /// Whether an instance with these world bounds is out of reach and can be
    /// skipped. Never for an unlimited reach.
    ///
    /// The box is tested rather than its centre, so a long building whose
    /// origin is far away is still collided while the player stands on one
    /// end of it. Exactly at the radius is within.
    [[nodiscard]] bool excludes(const glm::vec3& boundsMin, const glm::vec3& boundsMax) const {
        return limited_ && pointAABBDistanceSq(point_, boundsMin, boundsMax) > radius_ * radius_;
    }

    /// Whether geometry within `distance` of the point can be in reach - so
    /// whether a query there can find anything at all.
    [[nodiscard]] bool reaches(const glm::vec3& point, float distance) const {
        if (!limited_) return true;
        const glm::vec3 d = point - point_;
        const float limit = radius_ + distance;
        return glm::dot(d, d) <= limit * limit;
    }

private:
    bool limited_ = false;
    glm::vec3 point_{0.0f};
    float radius_ = 0.0f;
};

}  // namespace wowee::collision
