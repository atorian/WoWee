#pragma once

/// The light the frame carries while the camera is inside a WMO interior group.
///
/// The sun does not reach an interior group. The WMO shader lights one from
/// the building's MOHD ambient plus its baked MOCV vertex colours and ignores
/// the frame's light colour. Doodads and characters take the frame's light,
/// so without this the hall of Ironforge is lit by the building and everything
/// standing in it by the sun and the zone's LightParams.
///
/// MOHD's ambient is the only light the header gives. It replaces the ambient,
/// and the directional light goes to zero. Everything else - fog, sky colours,
/// the sun's direction - stays the outdoor light, which is what shows through
/// a door or a window.
///
/// An all-zero ambient is not taken. With no directional light beside it,
/// every model in the room would draw black; the zone's ambient is kept.

#include "rendering/lighting_manager.hpp"

#include <glm/glm.hpp>

namespace wowee::rendering {

inline void applyInteriorLight(LightingParams& params, const glm::vec3& mohdAmbient) {
    if (mohdAmbient != glm::vec3(0.0f)) params.ambientColor = mohdAmbient;
    params.diffuseColor = glm::vec3(0.0f);
}

}  // namespace wowee::rendering
