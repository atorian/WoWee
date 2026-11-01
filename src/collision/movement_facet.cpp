#include "collision/movement_facet.hpp"
#include <cmath>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define WOWEE_FACET_SSE 1
#endif

namespace wowee::collision {
std::optional<MovementFacetBuild> makeMovementFacet(
    const std::array<glm::vec3, 3>& vertices, const glm::mat4& transform,
    uint64_t owner, FacetNormalization normalization) {
#ifndef WOWEE_FACET_SSE
    if (normalization == FacetNormalization::NativeSse) return std::nullopt;
#endif
    MovementFacetBuild result;
    auto& facet = result.facet;
    facet.owner = owner;
    // The differences and cross product stay in x87 registers until the
    // transformed normal is stored. A float glm::cross loses those bits.
    const glm::dvec3 u = glm::dvec3(vertices[1]) - glm::dvec3(vertices[0]);
    const glm::dvec3 v = glm::dvec3(vertices[2]) - glm::dvec3(vertices[0]);
    const glm::dvec3 normal(u.y*v.z-u.z*v.y, u.z*v.x-u.x*v.z, u.x*v.y-u.y*v.x);
    auto& n = facet.plane.normal;
    for (int axis = 0; axis < 3; ++axis) {
        n[axis] = float((transform[2][axis]*normal.z + transform[1][axis]*normal.y) +
                        transform[0][axis]*normal.x);
        for (size_t i = 0; i < 3; ++i) {
            const auto& p = vertices[i];
            facet.vertices[i][axis] = float(((double(transform[2][axis])*p.z +
                double(transform[1][axis])*p.y) + double(transform[0][axis])*p.x) +
                transform[3][axis]);
        }
    }
    const double lengthSquared = (double(n.x)*n.x + double(n.y)*n.y) + double(n.z)*n.z;
    result.degenerate = lengthSquared == 0;
    if (result.degenerate) {
        n = {0, 0, 1};
    } else {
        double inverse;
#ifdef WOWEE_FACET_SSE
        if (normalization == FacetNormalization::NativeSse) {
            inverse = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(float(lengthSquared))));
        } else
#endif
        {
            inverse = 1.0 / std::sqrt(lengthSquared);
        }
        n.x = float(inverse * n.x);
        n.y = float(inverse * n.y);
        n.z = float(inverse * n.z);
    }
    const auto& p = facet.vertices[0];
    facet.plane.offset = float(-((double(p.z)*n.z + double(p.y)*n.y) + double(p.x)*n.x));
    return result;
}
} // namespace wowee::collision
