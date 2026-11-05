#pragma once

#include "foundation/event_bus.hpp"

#include <cstdint>
#include <array>
#include <functional>
#include <vector>

namespace wowee {

namespace rendering { class Renderer; }
namespace core { class EntitySpawner; }

namespace core {

/// Handles entity spawn/despawn callbacks: creatures, players, game objects.
class EntitySpawnCallbackHandler {
public:
    /// @param renderer Null in a client that draws nothing: what is in view is
    ///        still queued on the spawner, and nothing is animated or moved.
    /// @param isLocalPlayerGuid Returns true if the given GUID is the local player (to skip self-spawn)
    EntitySpawnCallbackHandler(EntitySpawner& entitySpawner,
                               rendering::Renderer* renderer,
                               EventBus& events,
                               std::function<bool(uint64_t)> isLocalPlayerGuid);

    void setupCallbacks();

private:
    EntitySpawner& entitySpawner_;
    rendering::Renderer* renderer_;
    EventBus& events_;
    std::function<bool(uint64_t)> isLocalPlayerGuid_;
    std::vector<EventBus::Subscription> subscriptions_;
};

} // namespace core
} // namespace wowee
