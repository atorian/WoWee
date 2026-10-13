#include "rendering/wall_sweep.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace wowee {
namespace rendering {

glm::vec3 sweepMoverAgainstWalls(const WMORenderer* wmo, const M2Renderer* m2,
                                 const glm::vec3& from, const glm::vec3& to,
                                 bool insideWMO) {
    const glm::vec3 delta = to - from;
    const float distSq = glm::dot(delta, delta);
    if (distSq <= 1e-4f) return to;   // stationary: not worth a collision call

    const float dist = std::sqrt(distSq);
    // Tighter steps indoors, where the geometry is closer together.
    const float stepSize = insideWMO ? 0.20f : 0.35f;
    const int steps = std::max(1, std::min(8, static_cast<int>(std::ceil(dist / stepSize))));
    const glm::vec3 stepDelta = delta / static_cast<float>(steps);

    glm::vec3 stepPos = from;
    for (int i = 0; i < steps; i++) {
        glm::vec3 candidate = stepPos + stepDelta;

        if (wmo) {
            glm::vec3 adjusted;
            if (wmo->checkWallCollision(stepPos, candidate, adjusted, insideWMO)) {
                candidate.x = adjusted.x;
                candidate.y = adjusted.y;
                // Up is a ramp; down would be a wall pulling the feet under the floor.
                candidate.z = std::max(candidate.z, adjusted.z);
            }
        }

        if (m2) {
            glm::vec3 adjusted;
            if (m2->checkCollision(stepPos, candidate, adjusted)) {
                candidate.x = adjusted.x;
                candidate.y = adjusted.y;
            }
        }

        stepPos = candidate;
    }
    return stepPos;
}

} // namespace rendering
} // namespace wowee
