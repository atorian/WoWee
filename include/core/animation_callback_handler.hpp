#pragma once

#include "core/event_bus.hpp"
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {

namespace rendering { class Renderer; }
namespace audio { class AudioCoordinator; }
namespace game { class GameHandler; }
namespace core { class AppearanceComposer; class EntitySpawner; }

namespace core {

class PlayerView;

/// Handles animation callbacks: death, respawn, swing, hit reaction, spell cast, emote,
/// stun, stealth, health, ghost, stand state, loot, sprint, vehicle, charge.
/// Owns charge rush state (interpolated in update).
class AnimationCallbackHandler {
public:
    AnimationCallbackHandler(EntitySpawner& entitySpawner,
                             rendering::Renderer& renderer,
                             PlayerView& playerView,
                             game::GameHandler& gameHandler,
                             AppearanceComposer& appearanceComposer,
                             audio::AudioCoordinator* audioCoordinator);

    void setupCallbacks();

    /// Called each frame from Application::update() to drive charge interpolation.
    /// Returns true if charge is active (player is externally driven).
    bool updateCharge(float deltaTime);

    // Charge state queries (used by Application::update for externallyDrivenMotion)
    [[nodiscard]] bool isCharging() const { return chargeActive_; }

    // Reset charge state (logout/disconnect)
    void resetChargeState();

private:
    EntitySpawner& entitySpawner_;
    rendering::Renderer& renderer_;
    PlayerView& playerView_;
    game::GameHandler& gameHandler_;
    AppearanceComposer& appearanceComposer_;
    /// The sound, for a potion's swig. Absent in a client built without it.
    audio::AudioCoordinator* audioCoordinator_;
    std::vector<EventBus::Subscription> subscriptions_;

    // Charge rush state (moved from Application)
    bool chargeActive_ = false;
    float chargeTimer_ = 0.0f;
    float chargeDuration_ = 0.0f;
    glm::vec3 chargeStartPos_{0.0f};  // Render coordinates
    glm::vec3 chargeEndPos_{0.0f};    // Render coordinates
    uint64_t chargeTargetGuid_ = 0;
};

} // namespace core
} // namespace wowee
