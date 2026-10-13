#pragma once
#include <cstdint>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio { class AudioCoordinator; }
namespace game { class ExpansionRegistry; class EmoteTable; class MovementCollision; }
namespace core { class EventBus; }

namespace game {

// Explicit service dependencies for game handlers.
// Owned by Application, passed by reference to GameHandler at construction.
// Replaces hidden Application::getInstance() singleton access.
//
// Nothing here draws: the game says what happened on `events`, and the
// client's presentation (core/) shows it. See AGENTS.md, "Layers".
struct GameServices {
    /// Still played directly by the game's handlers; moving to events.
    audio::AudioCoordinator* audioCoordinator = nullptr;
    pipeline::AssetManager* assetManager = nullptr;
    ExpansionRegistry* expansionRegistry = nullptr;
    /// What the emotes are. Required, like `events`.
    const EmoteTable* emotes = nullptr;
    /// Where the game announces what happened. Required: GameHandler
    /// refuses to be built without one.
    core::EventBus* events = nullptr;
    // The walls units moving themselves run into between their reports.
    const MovementCollision* movementCollision = nullptr;
    uint32_t gryphonDisplayId = 0;
    uint32_t wyvernDisplayId = 0;
};

} // namespace game
} // namespace wowee
