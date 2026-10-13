#pragma once

#include <glm/vec3.hpp>

namespace wowee {
namespace rendering {

class WMORenderer;
class M2Renderer;

/// Where a character-sized mover going straight from `from` to `to` ends up
/// against WMO walls and, given an M2 renderer, doodads: taken in steps short
/// enough not to tunnel through a wall, and slid along whatever it meets.
/// Render coordinates. Either renderer may be null.
///
/// insideWMO selects the tighter radius and finer steps used indoors, where
/// the geometry is closer together.
glm::vec3 sweepMoverAgainstWalls(const WMORenderer* wmo, const M2Renderer* m2,
                                 const glm::vec3& from, const glm::vec3& to,
                                 bool insideWMO);

} // namespace rendering
} // namespace wowee
