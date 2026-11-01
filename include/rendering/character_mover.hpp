#pragma once

#include <glm/glm.hpp>
#include <optional>

namespace wowee::rendering {

/// What the controls ask of the followed character this frame.
struct CharacterIntent {
    bool forward = false;
    bool backward = false;
    bool strafeLeft = false;
    bool strafeRight = false;
    bool turnLeft = false;
    bool turnRight = false;
    /// Jump pressed this frame.
    bool jump = false;
    /// Held: climbing or diving in water or in flight.
    bool ascend = false;
    bool descend = false;
    /// The view steers the character - the right mouse button, a finger on
    /// the view, swimming with a key held - and it faces this way, in the
    /// renderer's degrees, pitched this way in radians.
    std::optional<float> steeredFacingDeg;
    float steeredPitchRad = 0.0f;
    /// TOGGLERUN has the character walking.
    bool walk = false;
};

/// Where the character ended up, and what the camera and the animation read
/// of how.
struct CharacterMotion {
    glm::vec3 position{0.0f};
    float facingDeg = 0.0f;
    bool grounded = true;
    bool swimming = false;
    bool flying = false;
    /// Up is positive.
    float verticalVelocity = 0.0f;
};

/// Moves the followed character instead of the camera's own physics.
///
/// The camera asks every frame. While the answer is yes the character goes
/// where move() says and the camera only follows it: it sends no movement of
/// its own. While it is no, the camera moves the character as it always has.
class CharacterMover {
public:
    virtual ~CharacterMover() = default;
    /// Whether this frame's move is this mover's, for a character standing at
    /// `position` facing `facingDeg` - where the camera left it.
    [[nodiscard]] virtual bool movesCharacter(const glm::vec3& position, float facingDeg) = 0;
    [[nodiscard]] virtual CharacterMotion move(const CharacterIntent& intent) = 0;
};

}  // namespace wowee::rendering
