#pragma once

/**
 * shadow_cull_reuse.hpp - when a shadow map's last cull still holds.
 *
 * A shadow pass keeps the casters whose position lands inside the map's box
 * in its own clip space, each widened by a margin of its own - its bound
 * radius over the map's. Finding them walked the whole world every frame, two
 * hundred thousand doodads in Dalaran, for a few hundred casters; and the map
 * hardly moves from one frame to the next - the sun turns slowly, and the box
 * steps only when the player crosses a texel.
 *
 * So the walk is done with some slack, against the matrix of the moment, and
 * its list is tested exactly every frame until the map has moved by more than
 * the slack. Whether it has is exact, not a guess: the test a caster has to
 * pass is a box in the map's clip space, so everything that can pass it lies
 * in that box widened by the largest margin; if no corner of that region
 * moves by more than the slack from where the cached matrix put it, no caster
 * the new matrix keeps is missing from the cached list.
 */

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

/// Whether a cull against `cached`, done with `slack` to spare on every axis,
/// still holds every point that passes the box test against `now`: inside
/// [boxMin, boxMax] in `now`'s clip space, widened by a margin of at most
/// `maxMargin`. Both matrices are affine (orthographic maps).
[[nodiscard]] inline bool lightSpaceCullStillCovers(const glm::mat4& cached, const glm::mat4& now,
                                                    const glm::vec3& boxMin, const glm::vec3& boxMax,
                                                    float maxMargin, float slack) {
    // In double: a world a few thousand yards from its origin, through an
    // inverse, would otherwise spend the slack on rounding.
    const glm::dmat4 toWorld = glm::inverse(glm::dmat4(now));
    const glm::dmat4 drift = glm::dmat4(cached) - glm::dmat4(now);
    const glm::dvec3 lo = glm::dvec3(boxMin) - double(maxMargin);
    const glm::dvec3 hi = glm::dvec3(boxMax) + double(maxMargin);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::dvec4 clip((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                              (corner & 4) ? hi.z : lo.z, 1.0);
        const glm::dvec4 world = toWorld * clip;
        // The affine difference of the two maps at this corner, which is
        // where it is largest over the region.
        const glm::dvec4 moved = drift * world;
        if (std::max({std::abs(moved.x), std::abs(moved.y), std::abs(moved.z)}) > double(slack)) {
            return false;
        }
    }
    return true;
}

}  // namespace rendering
}  // namespace wowee
