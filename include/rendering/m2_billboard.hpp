#pragma once

// Billboard bones, as the client turns them (CM2Model::Animate, WoW.exe
// 3.3.5a 0x82f0f0, dispatch 0x82ff3a; render spec section 3.5).
//
// The client billboards in VIEW space, after the bone's whole parent chain and
// the model's placement, against the view's fixed axis rather than toward the
// eye point: every card of a model turns alike, as on a screen. A bone's axes
// in view space are rebuilt per its flag, each keeping its length, and the
// pivot is kept where it was. A card faces +X, Y left, Z up.
//
// The client's view space is left-handed (x right, y up, z into the screen);
// this one is right-handed (z out of the screen), so the cross products below
// run the other way round from the disassembly and +X comes out toward the
// viewer, as the client's constant basis has it.

#include <glm/glm.hpp>
#include <bit>
#include <cstdint>

namespace wowee::rendering {

/// M2 bone flags for billboards.
constexpr uint32_t kM2BoneSphericalBillboard = 0x8;    ///< faces the view on every axis
constexpr uint32_t kM2BoneCylindricalLockX = 0x10;     ///< keeps its X axis, turns about it
constexpr uint32_t kM2BoneCylindricalLockY = 0x20;     ///< keeps its Y axis, turns about it
constexpr uint32_t kM2BoneCylindricalLockZ = 0x40;     ///< keeps its Z axis: stays upright
constexpr uint32_t kM2BoneBillboardMask = kM2BoneSphericalBillboard | kM2BoneCylindricalLockX |
                                          kM2BoneCylindricalLockY | kM2BoneCylindricalLockZ;
/// The bits the client tests for a bone that moves by its own tracks (0x200
/// marks one with animation tracks): a spherical billboard of such a bone
/// still turns by its own animation.
constexpr uint32_t kM2BoneAnimatedMask = 0x280;

/// Normalised, unless too short to say a direction: then left as it is, zero
/// staying zero, and the card collapsing - the client has no fallback.
inline glm::vec3 m2BillboardNormalize(const glm::vec3& v) {
    const float len2 = glm::dot(v, v);
    return len2 > 2.384e-7f ? v * glm::inversesqrt(len2) : v;
}

/// A billboard bone's matrix in view space.
///
/// `boneView` is the bone's matrix in view space as animated (view x model x
/// parent chain x its own), `local` its own rotation and scale, `pivot` its
/// pivot in model space.
inline glm::mat4 m2BillboardBone(uint32_t boneFlags, const glm::mat4& boneView,
                                 const glm::mat3& local, const glm::vec3& pivot) {
    const glm::vec3 s(glm::length(glm::vec3(boneView[0])), glm::length(glm::vec3(boneView[1])),
                      glm::length(glm::vec3(boneView[2])));
    const glm::vec3 pivotView = glm::vec3(boneView * glm::vec4(pivot, 1.0f));
    glm::vec3 c0(boneView[0]), c1(boneView[1]), c2(boneView[2]);
    const uint32_t kinds = boneFlags & kM2BoneBillboardMask;
    if (std::popcount(kinds) > 1) {
        // Two kinds at once rebuild nothing and only rescale, which squares
        // the axes' lengths - as the client does.
    } else if (kinds == kM2BoneCylindricalLockX) {
        c0 = m2BillboardNormalize(c0);
        c1 = m2BillboardNormalize(glm::vec3(c0.y, -c0.x, 0.0f));
        c2 = glm::cross(c0, c1);
    } else if (kinds == kM2BoneCylindricalLockY) {
        c1 = m2BillboardNormalize(c1);
        c0 = m2BillboardNormalize(glm::vec3(-c1.y, c1.x, 0.0f));
        c2 = glm::cross(c0, c1);
    } else if (kinds == kM2BoneCylindricalLockZ) {
        c2 = m2BillboardNormalize(c2);
        c1 = m2BillboardNormalize(glm::vec3(c2.y, -c2.x, 0.0f));
        c0 = glm::cross(c1, c2);
    } else if (kinds == kM2BoneSphericalBillboard) {
        // Every rotation above it dropped: X toward the viewer, Y to the
        // right, Z up - turned by the bone's own animation if it has any.
        if (boneFlags & kM2BoneAnimatedMask) {
            c0 = m2BillboardNormalize(glm::vec3(local[0].y, local[0].z, local[0].x));
            c1 = m2BillboardNormalize(glm::vec3(local[1].y, local[1].z, local[1].x));
            c2 = m2BillboardNormalize(glm::vec3(local[2].y, local[2].z, local[2].x));
        } else {
            c0 = glm::vec3(0.0f, 0.0f, 1.0f);
            c1 = glm::vec3(1.0f, 0.0f, 0.0f);
            c2 = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    } else {
        return boneView;
    }
    c0 *= s.x;
    c1 *= s.y;
    c2 *= s.z;
    glm::mat4 out(1.0f);
    out[0] = glm::vec4(c0, 0.0f);
    out[1] = glm::vec4(c1, 0.0f);
    out[2] = glm::vec4(c2, 0.0f);
    out[3] = glm::vec4(pivotView - (pivot.x * c0 + pivot.y * c1 + pivot.z * c2), 1.0f);
    return out;
}

} // namespace wowee::rendering
