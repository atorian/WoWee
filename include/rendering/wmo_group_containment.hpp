#pragma once

#include <cstdint>

#include "pipeline/wmo_loader.hpp"

#include <glm/glm.hpp>

namespace wowee::rendering {

using pipeline::kWMOGroupIndoor;

/// Whether a group's model-space box holds a model-space point, counting
/// interior groups only when `interiorOnly` is set. Interior groups are what
/// decide whether the sky is still drawn and whether a model is lit by the
/// building or the sun. Stormwind's streets are exterior groups of one
/// city-wide WMO, so "inside a group" alone puts the whole city indoors.
template <class Group>
[[nodiscard]] bool groupContainsPoint(const Group& group, const glm::vec3& localPos,
                                      bool interiorOnly) {
    if (interiorOnly && !(group.groupFlags & kWMOGroupIndoor)) return false;
    return localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
           localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
           localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z;
}

}  // namespace wowee::rendering
