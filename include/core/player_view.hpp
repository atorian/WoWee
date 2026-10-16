#pragma once

#include "core/event_bus.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace wowee {
namespace addons { struct LuaServices; }
namespace audio { class AudioCoordinator; }
namespace game { class GameHandler; }
namespace rendering {
class Camera;
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
///
/// The controls and the camera exist without a renderer: a client that draws
/// nothing still walks when a movement key is held, and still tells the
/// server. What only a drawn character has - its facing, its follow target,
/// the world's collision - waits for one.
class PlayerView {
public:
    /// Steers `camera` - the renderer's, when there is one. Hears the game turn
    /// the player and get them drunk. `renderer` is null in a client that draws
    /// nothing.
    PlayerView(rendering::Camera& camera, rendering::Renderer* renderer,
               audio::AudioCoordinator* audio, rendering::SpellVisualSystem* spellVisuals,
               EventBus& events);
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

    /// What the movement key bindings hold - MoveForwardStart and the rest,
    /// autorun, walk, zoom - given to the interface through its services.
    void bindMovementControls(addons::LuaServices& services);
    /// On entering the world: the camera's movement, sitting and standing,
    /// and the end of an auto-follow, reach the game.
    void connectMovement(game::GameHandler& game);

    /// Present, first: the controls move the camera and the player, and the
    /// movement that starts or stops is sent.
    void updateControls(float deltaTime);
    /// Before the renderer's update, after the controls: turn and animate the
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

    rendering::Renderer* renderer_;
    rendering::SpellVisualSystem* spellVisuals_;
    std::unique_ptr<rendering::CameraController> cameraController_;
    std::unique_ptr<rendering::AnimationController> animationController_;
    double lastCameraUpdateMs_ = 0.0;
    std::vector<EventBus::Subscription> subscriptions_;
};

} // namespace core
} // namespace wowee
