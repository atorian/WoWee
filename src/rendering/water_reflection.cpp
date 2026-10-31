#include "rendering/water_reflection.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace wowee {
namespace rendering {

glm::mat4 mirroredView(glm::vec3 cameraPos, glm::vec3 forward, float waterHeight) {
    glm::vec3 mirroredPos = cameraPos;
    mirroredPos.z = 2.0f * waterHeight - cameraPos.z;
    forward.z = -forward.z;
    return glm::lookAt(mirroredPos, mirroredPos + forward, glm::vec3(0.0f, 0.0f, 1.0f));
}

glm::mat4 obliqueReflectionProjection(const glm::mat4& projection, const glm::mat4& view,
                                      float waterHeight) {
    // The plane z = waterHeight, facing up, in the mirrored camera's space.
    const glm::vec4 planeWorld(0.0f, 0.0f, 1.0f, -waterHeight);
    const glm::vec4 c = glm::transpose(glm::inverse(view)) * planeWorld;

    // Lengyel's oblique near plane. Clip depth here runs from 0 at the near
    // plane to w at the far one, so the depth row becomes the plane itself,
    // scaled so that the far corner of the frustum on the plane's side still
    // lands on the far plane: Q is that corner, back from clip space. The
    // form taken here before was the one for -w to w depth, which put the
    // near plane nowhere near the water, and the bed under the sea came
    // through in the reflection as dark ground. The corner is picked by the
    // plane's signs in clip space, not the camera's: this projection flips y.
    const glm::mat4 inverseProjection = glm::inverse(projection);
    const glm::vec4 cClip = glm::transpose(inverseProjection) * c;
    const glm::vec4 q = inverseProjection *
                        glm::vec4(glm::sign(cClip.x), glm::sign(cClip.y), 1.0f, 1.0f);
    const glm::vec4 m = c * (1.0f / glm::dot(c, q));
    glm::mat4 result = projection;
    result[0][2] = m.x;
    result[1][2] = m.y;
    result[2][2] = m.z;
    result[3][2] = m.w;
    return result;
}

namespace {

/// Surfaces this close in height share a plane: the chunks of one lake are
/// cut apart a little above or below each other.
constexpr float kPlaneTolerance = 1.0f;
/// How far above the plane the camera must be to see a mirror in it, as
/// renderReflectionPass asked before the plane was chosen here.
constexpr float kCameraAbovePlane = 0.5f;
/// How far past the water's own image a pixel of it reads the mirror, in
/// parts of the image: water.frag shifts the lookup by up to 0.015 with the
/// ripples' normal, and a little more is kept for the waves' height.
constexpr float kAreaMargin = 0.02f;
/// Clip w below which a corner counts as behind the camera.
constexpr float kInFront = 1.0e-3f;

/// The part of the horizontal rectangle `lo`..`hi` at height `z` in front of
/// the camera of `viewProj`, as the extent it covers in its image: x0, y0,
/// x1, y1 from -1 to 1. None when none of it is in front, or none in the
/// picture.
std::optional<glm::vec4> imageExtent(const glm::mat4& viewProj, glm::vec2 lo, glm::vec2 hi, float z) {
    const std::array<glm::vec4, 4> corners = {
        viewProj * glm::vec4(lo.x, lo.y, z, 1.0f), viewProj * glm::vec4(hi.x, lo.y, z, 1.0f),
        viewProj * glm::vec4(hi.x, hi.y, z, 1.0f), viewProj * glm::vec4(lo.x, hi.y, z, 1.0f)};
    // The rectangle cut at the plane just in front of the camera
    // (Sutherland-Hodgman against w), so a lake the camera stands at the edge
    // of is measured by the part of it ahead.
    std::vector<glm::vec4> ahead;
    for (size_t i = 0; i < corners.size(); ++i) {
        const glm::vec4& a = corners[i];
        const glm::vec4& b = corners[(i + 1) % corners.size()];
        const bool aIn = a.w >= kInFront, bIn = b.w >= kInFront;
        if (aIn) ahead.push_back(a);
        if (aIn != bIn) ahead.push_back(glm::mix(a, b, (kInFront - a.w) / (b.w - a.w)));
    }
    if (ahead.empty()) return std::nullopt;
    glm::vec2 low(std::numeric_limits<float>::max()), high(-std::numeric_limits<float>::max());
    for (const glm::vec4& c : ahead) {
        const glm::vec2 p = glm::vec2(c) / c.w;
        low = glm::min(low, p);
        high = glm::max(high, p);
    }
    if (high.x < -1.0f || low.x > 1.0f || high.y < -1.0f || low.y > 1.0f) return std::nullopt;
    return glm::vec4(glm::clamp(low, -1.0f, 1.0f), glm::clamp(high, -1.0f, 1.0f));
}

float extentArea(const glm::vec4& e) { return (e.z - e.x) * (e.w - e.y); }

glm::vec4 unite(const std::optional<glm::vec4>& a, const glm::vec4& b) {
    if (!a) return b;
    return glm::vec4(glm::min(glm::vec2(*a), glm::vec2(b)), glm::max(glm::vec2(a->z, a->w), glm::vec2(b.z, b.w)));
}

} // namespace

bool WaterReflectionView::canSee(const glm::vec3& boxMin, const glm::vec3& boxMax) const {
    return boxMax.z >= height && seen.intersectsAABB(boxMin, boxMax);
}

bool WaterReflectionView::canSee(const glm::vec3& centre, float radius) const {
    return centre.z + radius >= height && seen.intersectsSphere(centre, radius);
}

bool WaterReflectionView::mirrors(const WaterInView& water) const {
    return std::abs(water.height - height) <= kPlaneTolerance;
}

std::optional<WaterReflectionView> waterReflectionView(const ReflectedCamera& camera,
                                                       std::span<const WaterInView> water) {
    // How much of the picture each surface covers, below the camera.
    const glm::mat4 cameraViewProj = camera.projection * camera.view;
    std::vector<float> covers(water.size(), 0.0f);
    for (size_t i = 0; i < water.size(); ++i) {
        const WaterInView& w = water[i];
        if (!w.seen || camera.position.z < w.height + kCameraAbovePlane) continue;
        if (const auto e = imageExtent(cameraViewProj, glm::vec2(w.min), glm::vec2(w.max), w.height))
            covers[i] = extentArea(*e);
    }
    // The plane whose water covers most of it.
    std::optional<float> plane;
    float best = 0.0f;
    for (size_t i = 0; i < water.size(); ++i) {
        if (covers[i] <= 0.0f) continue;
        float sum = 0.0f;
        for (size_t j = 0; j < water.size(); ++j) {
            if (std::abs(water[j].height - water[i].height) <= kPlaneTolerance) sum += covers[j];
        }
        if (sum > best) {
            best = sum;
            plane = water[i].height;
        }
    }
    if (!plane) return std::nullopt;

    WaterReflectionView out;
    out.height = *plane;
    out.view = mirroredView(camera.position, camera.forward, out.height);
    out.projection = obliqueReflectionProjection(camera.projection, out.view, out.height);

    // Where the water on the plane lies in the mirrored image, from the
    // lowest its waves reach to the highest. The oblique projection moves
    // only depth, so the camera's own projection places it the same.
    const glm::mat4 mirroredViewProj = camera.projection * out.view;
    std::optional<glm::vec4> extent;
    for (const WaterInView& w : water) {
        if (!w.seen || !out.mirrors(w)) continue;
        for (const float z : {w.min.z, w.max.z}) {
            if (const auto e = imageExtent(mirroredViewProj, glm::vec2(w.min), glm::vec2(w.max), z))
                extent = unite(extent, *e);
        }
    }
    if (!extent) return std::nullopt;
    const glm::vec4 margin(-2.0f * kAreaMargin, -2.0f * kAreaMargin, 2.0f * kAreaMargin, 2.0f * kAreaMargin);
    const glm::vec4 ndc = glm::clamp(*extent + margin, glm::vec4(-1.0f), glm::vec4(1.0f));
    out.area = ndc * 0.5f + 0.5f;

    // The mirrored camera narrowed to that part of its image: clip x and y
    // scaled so the area's edges land on the frustum's sides.
    const glm::vec2 centre = (glm::vec2(ndc.x, ndc.y) + glm::vec2(ndc.z, ndc.w)) * 0.5f;
    const glm::vec2 scale = 2.0f / glm::max(glm::vec2(ndc.z - ndc.x, ndc.w - ndc.y), glm::vec2(1.0e-4f));
    glm::mat4 crop(1.0f);
    crop[0][0] = scale.x;
    crop[1][1] = scale.y;
    crop[3][0] = -scale.x * centre.x;
    crop[3][1] = -scale.y * centre.y;
    out.seen.extractFromMatrix(crop * mirroredViewProj);
    return out;
}

} // namespace rendering
} // namespace wowee
