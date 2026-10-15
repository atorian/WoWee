#pragma once

#include "core/event_bus.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace wowee {
namespace audio { class AudioCoordinator; }
namespace rendering {
class Renderer;
class CameraController;
class AnimationController;
class SpellVisualSystem;
struct ViewerMotion;
}

namespace core {

/// The local player as the client presents them: the camera that follows and
/// steers them, the way their character faces, and how it animates.
///
/// The renderer draws a character at a position with a yaw and is told how
/// the viewer moves; this decides all three, from the camera controller's
/// input and the animation controller's state. Owned by the Application.
class PlayerView {
public:
    /// Hears the game turn the player and get them drunk.
    PlayerView(rendering::Renderer& renderer, audio::AudioCoordinator* audio,
               rendering::SpellVisualSystem* spellVisuals, EventBus& events);
    ~PlayerView();

    PlayerView(const PlayerView&) = delete;
    PlayerView& operator=(const PlayerView&) = delete;

    /// Once the world's renderers exist: the camera collides with what they
    /// hold, and is put back on the ground. Again after every world load.
    void attachWorld();
    /// The map's terrain is gone with its scope: the camera stops colliding
    /// with it until the next map attaches its own.
    void detachWorld();

    /// Follow this character instance; 0 for none.
    void setCharacterFollow(uint32_t instanceId);

    /// Before the renderer's update: move the camera, turn and animate the
    /// character, and tell the renderer how the viewer moves.
    void beforeRendererUpdate(float deltaTime);
    /// After it: the footsteps and the sounds the character makes, which
    /// query the doodads the renderer has just finished updating.
    void afterRendererUpdate(float deltaTime);

    /// A death, a teleport, a world change: the combat stance, the melee
    /// swings and the spell visuals in flight are dropped.
    void resetCombatVisualState();

    rendering::CameraController* getCameraController() { return cameraController_.get(); }
    rendering::AnimationController* getAnimationController() { return animationController_.get(); }
    /// How long the camera controller's last update took, in milliseconds.
    [[nodiscard]] double lastCameraUpdateMs() const { return lastCameraUpdateMs_; }

private:
    void updateCharacterFacing(float deltaTime);
    void spawnMountDust();
    [[nodiscard]] rendering::ViewerMotion viewerMotion() const;

    rendering::Renderer& renderer_;
    rendering::SpellVisualSystem* spellVisuals_;
    std::unique_ptr<rendering::CameraController> cameraController_;
    std::unique_ptr<rendering::AnimationController> animationController_;
    double lastCameraUpdateMs_ = 0.0;
    std::vector<EventBus::Subscription> subscriptions_;
};

} // namespace core
} // namespace wowee
