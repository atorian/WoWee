#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game { class ExpansionRegistry; class EmoteTable; class MovementCollision; }
namespace core { class EventBus; class CVarStore; }
namespace network { class WorldConnection; }

namespace game {

// Explicit service dependencies for game handlers.
// Owned by Application, passed by reference to GameHandler at construction.
// Replaces hidden Application::getInstance() singleton access.
//
// Nothing here draws or sounds: the game says what happened on `events`, and
// the client's presentation (core/) shows and plays it. See AGENTS.md,
// "Layers".
struct GameServices {
    pipeline::AssetManager* assetManager = nullptr;
    ExpansionRegistry* expansionRegistry = nullptr;
    /// What the emotes are. Required, like `events`.
    const EmoteTable* emotes = nullptr;
    /// Where the game announces what happened. Required: GameHandler
    /// refuses to be built without one.
    core::EventBus* events = nullptr;
    /// The CVars the game's own behaviour follows - autoClearAFK,
    /// assistAttack, autoDismountFlying. Required, like `events`; the client
    /// owns the store and the interface writes the same one.
    const core::CVarStore* cvars = nullptr;
    /// A new connection to a world server, one per login. Required: the
    /// client makes a network::WorldSocket, a test one it can play packets
    /// into and read what was sent from.
    std::function<std::unique_ptr<network::WorldConnection>()> makeConnection;
    // The walls units moving themselves run into between their reports.
    const MovementCollision* movementCollision = nullptr;
    /// Where the account data - key bindings, macros, settings - is kept on
    /// disk, one folder per account and server. Empty keeps none, which
    /// AccountDataHandler says once.
    std::string accountDataDir;
    /// The monotonic clock the realm's time is kept running on between the
    /// packets that set it. Real time in the client; a test hands over its
    /// own, so time passes when it says.
    std::function<std::chrono::steady_clock::time_point()> steadyNow =
        [] { return std::chrono::steady_clock::now(); };
    uint32_t gryphonDisplayId = 0;
    uint32_t wyvernDisplayId = 0;
};

} // namespace game
} // namespace wowee
