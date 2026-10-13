#pragma once

#include "core/event_bus.hpp"

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace audio { class AudioCoordinator; }
namespace pipeline { class AssetManager; }
namespace rendering { class Renderer; class SpellVisualSystem; }
namespace game { class GameHandler; }

namespace core {

class EntitySpawner;

/// How spells look and sound: cast and impact effects, the shot of a ranged
/// attack, the looks auras give the units wearing them, and the cast, impact,
/// swing and eating sounds.
///
/// The game says what happened to a spell (game/game_events.hpp); this
/// decides what the player sees and hears of it. It reads the game's state -
/// a spell's school and visual, a unit's position and auras - and never
/// changes it. Owned by the Application.
class SpellPresentation {
public:
    SpellPresentation(EventBus& events, game::GameHandler& game,
                      rendering::Renderer& renderer, EntitySpawner& spawner,
                      rendering::SpellVisualSystem* visuals,
                      audio::AudioCoordinator* audio,
                      pipeline::AssetManager& assets);

    SpellPresentation(const SpellPresentation&) = delete;
    SpellPresentation& operator=(const SpellPresentation&) = delete;

    /// Once a frame, after the game's update: aura looks and the eating and
    /// drinking sound.
    void update(float deltaTime);

private:
    void onCastStarted(uint64_t caster, uint32_t spellId, uint32_t castTimeMs,
                       bool byPlayer, bool isProfession, bool isRangedAutoAttack);
    void onCastSucceeded(uint64_t caster, uint32_t spellId, bool byPlayer,
                         bool isProfession, bool isRangedAutoAttack, bool isMeleeAbility,
                         const std::vector<uint64_t>& hitTargets, uint64_t projectileTarget);

    void syncAuraLooks(float deltaTime);
    void tickRestorationSound(float deltaTime);

    void playCastSound(uint32_t spellId);
    void playImpactSound(uint32_t spellId);
    void playImpactVisual(uint32_t spellId, uint64_t target);
    void launchRangedProjectile(uint32_t spellId, uint64_t target);

    /// Spell.dbc's SpellVisualID, or 0.
    [[nodiscard]] uint32_t visualIdFor(uint32_t spellId) const;
    /// Where a unit is drawn, in render space.
    bool unitPosition(uint64_t guid, glm::vec3& out) const;
    /// The character instance a unit is drawn as, for effects attached to its
    /// bones; 0 when it is not drawn.
    [[nodiscard]] uint32_t unitInstance(uint64_t guid) const;

    game::GameHandler& game_;
    rendering::Renderer& renderer_;
    EntitySpawner& spawner_;
    rendering::SpellVisualSystem* visuals_;
    audio::AudioCoordinator* audio_;
    pipeline::AssetManager& assets_;

    float auraLookTimer_ = 0.0f;
    bool wasRestoring_ = false;
    float restorationSoundTimer_ = 0.0f;

    std::vector<EventBus::Subscription> subscriptions_;
};

} // namespace core
} // namespace wowee
