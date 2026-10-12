#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// MOGP flag 0x2000: the group is indoors. It is what separates a
/// building's inside from the porch and the roof, which are groups of the
/// same model, and so what decides whether the sky is still drawn and
/// whether the frame is lit by the building or the sun. Stormwind's streets
/// are exterior groups of one city-wide WMO, so "inside a group" alone puts
/// the whole city indoors.
inline constexpr uint32_t kWMOGroupIndoor = 0x2000;

/// Whether a group's model-space box holds a model-space point, counting
/// interior groups only when `interiorOnly` is set.
template <class Group>
[[nodiscard]] bool groupContainsPoint(const Group& group, const glm::vec3& localPos,
                                      bool interiorOnly) {
    if (interiorOnly && !(group.groupFlags & kWMOGroupIndoor)) return false;
    return localPos.x >= group.boundingBoxMin.x && localPos.x <= group.boundingBoxMax.x &&
           localPos.y >= group.boundingBoxMin.y && localPos.y <= group.boundingBoxMax.y &&
           localPos.z >= group.boundingBoxMin.z && localPos.z <= group.boundingBoxMax.z;
}

}  // namespace wowee::rendering
