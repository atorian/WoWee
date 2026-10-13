#include "core/player_view.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/game_events.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/mount_dust.hpp"
#include "rendering/renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/viewer_motion.hpp"

#include <chrono>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee {
namespace core {

PlayerView::PlayerView(rendering::Renderer& renderer, audio::AudioCoordinator* audio,
                       rendering::SpellVisualSystem* spellVisuals, EventBus& events)
    : renderer_(renderer),
      spellVisuals_(spellVisuals),
      cameraController_(std::make_unique<rendering::CameraController>(renderer.getCamera())),
      animationController_(std::make_unique<rendering::AnimationController>()) {
    cameraController_->setUseWoWSpeed(true);  // Use realistic WoW movement speed
    cameraController_->setMouseSensitivity(0.15f);
    animationController_->initialize(&renderer_, cameraController_.get(), audio, spellVisuals_);

    namespace ev = game::events;
    subscriptions_.push_back(events.subscribe<ev::PlayerIntoxicationChanged>(
        [this](const ev::PlayerIntoxicationChanged& e) {
            cameraController_->setIntoxication(e.amount);
            if (auto* post = renderer_.getPostProcessPipeline()) post->setIntoxication(e.amount);
        }));
    subscriptions_.push_back(events.subscribe<ev::PlayerFacingSet>([this](const ev::PlayerFacingSet& e) {
        // A one-shot check that the drawn facing round-trips to the yaw the game
        // computed. If it does not, the per-frame resync replaces it with a
        // different heading and the server checks the arc against that instead.
        const float fromRender = coords::characterYawDegToCanonical(renderer_.getCharacterYaw());
        const float delta = coords::normalizeAngleRad(fromRender - e.canonicalYaw);
        if (std::abs(delta) > 0.2f) {
            LOG_WARNING("Facing mismatch: computed canonical=", e.canonicalYaw,
                        " but the character's ", renderer_.getCharacterYaw(),
                        " deg maps to ", fromRender, " (off by ",
                        delta * 57.2957795f, " deg) - the resync will undo this");
        }
        // The drawn character owns facing; the game reads it back every frame.
        const float facingDeg = coords::canonicalToCharacterYawDeg(e.canonicalYaw);
        renderer_.setCharacterYaw(facingDeg);
        cameraController_->setFacingYaw(facingDeg);
    }));
}

PlayerView::~PlayerView() = default;

void PlayerView::attachWorld() {
    cameraController_->setTerrainManager(renderer_.getTerrainManager());
    cameraController_->setWMORenderer(renderer_.getWMORenderer());
    cameraController_->setM2Renderer(renderer_.getM2Renderer());
    cameraController_->setWaterRenderer(renderer_.getWaterRenderer());
    // Snap camera to ground
    cameraController_->reset();
}

void PlayerView::setCharacterFollow(uint32_t instanceId) {
    renderer_.setCharacterFollow(instanceId);
    if (instanceId > 0) cameraController_->setFollowTarget(&renderer_.getCharacterPosition());
    animationController_->onCharacterFollow(instanceId);
}

void PlayerView::resetCombatVisualState() {
    animationController_->resetCombatVisualState();
    if (spellVisuals_) spellVisuals_->reset();
}

rendering::ViewerMotion PlayerView::viewerMotion() const {
    rendering::ViewerMotion motion;
    motion.thirdPerson = cameraController_->isThirdPerson() && cameraController_->getFollowTarget();
    motion.moving = cameraController_->isMoving();
    motion.swimming = cameraController_->isSwimming();
    motion.yawDeg = cameraController_->getYaw();
    motion.insideInterior = cameraController_->isInsideInteriorWMO();
    return motion;
}

void PlayerView::beforeRendererUpdate(float deltaTime) {
    const auto cameraStart = std::chrono::steady_clock::now();
    cameraController_->update(deltaTime);
    const double cameraMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cameraStart).count();
    if (cameraMs > 50.0) {
        LOG_WARNING("SLOW cameraController->update: ", cameraMs, "ms");
    }
    lastCameraUpdateMs_ = cameraMs;

    renderer_.setViewerMotion(viewerMotion());

    // Visibility hardening: ensure player instance cannot stay hidden after
    // taxi/camera transitions, but preserve first-person self-hide.
    auto* characterRenderer = renderer_.getCharacterRenderer();
    const uint32_t characterInstanceId = renderer_.getCharacterInstanceId();
    if (characterRenderer && characterInstanceId > 0) {
        if ((cameraController_->isThirdPerson() && !cameraController_->isFirstPersonView()) ||
            animationController_->isTaxiFlight()) {
            characterRenderer->setInstanceVisible(characterInstanceId, true);
        }
    }

    updateCharacterFacing(deltaTime);
    spawnMountDust();
    if (spellVisuals_) spellVisuals_->update(deltaTime);
}

void PlayerView::afterRendererUpdate(float deltaTime) {
    // Footsteps: authored footfall events add new prints. After the renderer's
    // update, whose M2 work rebuilds the spatial index the floor query walks.
    animationController_->updateFootsteps(deltaTime);
    // Activity SFX + mount ambient sounds
    animationController_->updateSfxState(deltaTime);
}

void PlayerView::updateCharacterFacing(float deltaTime) {
    auto* characterRenderer = renderer_.getCharacterRenderer();
    const uint32_t characterInstanceId = renderer_.getCharacterInstanceId();
    if (characterInstanceId == 0 || !characterRenderer) return;

    const glm::vec3& characterPosition = renderer_.getCharacterPosition();
    float characterYaw = renderer_.getCharacterYaw();
    auto* cc = cameraController_.get();
    auto* anim = animationController_.get();

    characterRenderer->setInstancePosition(characterInstanceId, characterPosition);

    // Movement-facing comes from camera controller and is decoupled from LMB orbit.
    const bool taxiFlight = anim->isTaxiFlight();
    const bool activeStrafe = (cc->isStrafingLeft() || cc->isStrafingRight()) && !cc->isMovingBackward();
    float torsoYawDeltaDeg = 0.0f;
    if (taxiFlight) {
        characterYaw = cc->getFacingYaw();
    } else if (cc->isMoving() && activeStrafe) {
        characterYaw = cc->getTravelYaw();
        torsoYawDeltaDeg = cc->getFacingYaw() - characterYaw;
    } else if (cc->isMoving() || cc->isRightMouseHeld() ||
               cc->isTurningLeft() || cc->isTurningRight()) {
        characterYaw = cc->getFacingYaw();
    } else if (anim->isInCombat() && anim->getTargetPosition() && !anim->isEmoteActive() &&
               !anim->isMounted()) {
        glm::vec3 toTarget = *anim->getTargetPosition() - characterPosition;
        if (toTarget.x * toTarget.x + toTarget.y * toTarget.y > 0.01f) {
            // Go through canonical, the way spawning and the camera do.
            // Taking atan2 of the render delta directly yields a heading in
            // a different convention - a mirror about 135 degrees - so the
            // spin looked roughly right but the frame loop then converted it
            // back to a canonical yaw that pointed somewhere else, and the
            // server rejected the cast for not facing the target.
            const glm::vec3 toTargetCanonical = coords::renderToCanonical(toTarget);
            const float canonYawToTarget = std::atan2(-toTargetCanonical.y, toTargetCanonical.x);
            const float targetYaw = coords::canonicalToCharacterYawDeg(canonYawToTarget);
            float diff = targetYaw - characterYaw;
            while (diff > 180.0f) diff -= 360.0f;
            while (diff < -180.0f) diff += 360.0f;
            const float rotSpeed = 360.0f * deltaTime;
            if (std::abs(diff) < rotSpeed) {
                characterYaw = targetYaw;
            } else {
                characterYaw += (diff > 0 ? rotSpeed : -rotSpeed);
            }
        }
    }
    renderer_.setCharacterYaw(characterYaw);
    characterRenderer->setInstanceRotation(characterInstanceId,
                                           glm::vec3(0.0f, 0.0f, glm::radians(characterYaw)));

    while (torsoYawDeltaDeg > 180.0f) torsoYawDeltaDeg -= 360.0f;
    while (torsoYawDeltaDeg < -180.0f) torsoYawDeltaDeg += 360.0f;
    characterRenderer->setInstanceTorsoYaw(characterInstanceId, glm::radians(torsoYawDeltaDeg));

    // Update animation based on movement state
    anim->updateMeleeTimers(deltaTime);
    anim->setDeltaTime(deltaTime);
    anim->updateCharacterAnimation();
}

void PlayerView::spawnMountDust() {
    // Dust when mounted and moving on the ground.
    auto* mountDust = renderer_.getMountDust();
    const auto* camera = renderer_.getCamera();
    auto* anim = animationController_.get();
    if (!mountDust || !camera || !anim->isMounted() || anim->isTaxiFlight()) return;
    if (!cameraController_->isMoving() || !cameraController_->isGrounded()) return;

    // Velocity from the camera's direction and speed, along the ground.
    glm::vec3 velocity = camera->getForward() * cameraController_->getMovementSpeed();
    velocity.z = 0.0f;
    // At the mount's feet, slightly below the character position.
    const glm::vec3 dustPos = renderer_.getCharacterPosition() -
                              glm::vec3(0.0f, 0.0f, anim->getMountHeightOffset() * 0.8f);
    mountDust->spawnDust(dustPos, velocity, true);
}

} // namespace core
} // namespace wowee
