#pragma once

namespace wowee {
namespace rendering {

/// How the viewer moves through the world this frame, for what the renderer
/// draws in answer: swim spray and ripples, the wake on the water, the
/// minimap's centre, the mist thinning indoors.
///
/// Told by whoever moves the camera - the client, from its camera controller -
/// once a frame before update(). A still camera with no character keeps the
/// defaults.
struct ViewerMotion {
    /// The camera stands behind a character at the renderer's character
    /// position, and what the character does is drawn there rather than at
    /// the eye.
    bool thirdPerson = false;
    bool moving = false;
    bool swimming = false;
    /// The camera's heading in degrees, which way a moving viewer travels.
    float yawDeg = 0.0f;
    /// The camera is inside one of a WMO's interior groups.
    bool insideInterior = false;
};

} // namespace rendering
} // namespace wowee
