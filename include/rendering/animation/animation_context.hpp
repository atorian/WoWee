#pragma once

namespace wowee {
namespace audio { class AudioCoordinator; }
namespace rendering {

class Renderer;
class CameraController;

/// What the character's animation acts through: the renderer that draws the
/// character, the controller that moves it, and the sound it makes. Handed
/// over by whoever owns all three - the client - since none owns the others.
struct AnimationContext {
    Renderer* renderer = nullptr;
    CameraController* cameraController = nullptr;
    audio::AudioCoordinator* audio = nullptr;
};

} // namespace rendering
} // namespace wowee
