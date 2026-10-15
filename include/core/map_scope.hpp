#pragma once

#include "core/entity_spawner.hpp"
#include "core/job_group.hpp"
#include "core/world_loader.hpp"
#include <glm/glm.hpp>
#include <memory>

namespace wowee {
namespace rendering { class Renderer; class MapRenderSession; }
namespace game { class GameHandler; }

namespace core {

class PlayerView;

/// Everything the client holds while it is on one map.
///
/// Entering a map - login, a transfer, an instance, a far teleport within the
/// same map - destroys the previous scope and builds a new one; logout and
/// disconnect destroy it. Nothing is cleared by hand: what belongs to the map
/// lives here and goes with it, so leaving by any route releases the same
/// things.
///
/// Process-wide caches stay outside: the renderers' models and textures, the
/// spawner's DBC lookups and parsed models. Only the map's work, instances,
/// queues and per-object state live here.
///
/// Created and destroyed only in the Transitions phase of the client frame.
class MapScope {
public:
    struct Services {
        EntitySpawner& spawner;
        /// Null in a client that draws nothing; the scene then has no render
        /// session.
        rendering::Renderer* renderer = nullptr;
        game::GameHandler& game;
        PlayerView& playerView;
    };

    /// Entering a map: starts its load on this scope's jobs.
    MapScope(const Services& services, WorldLoader& loader, WorldLoader::Entry entry);
    /// A scene with nothing to load: an offline probe that places its own
    /// models.
    MapScope(const Services& services, uint32_t mapId);

    /// Cancels and joins this map's work, stops terrain streaming, lets go of
    /// what refers to the scene, then releases its instances. GPU resources
    /// retire behind the frame fences; nothing waits for the device.
    ~MapScope();

    MapScope(const MapScope&) = delete;
    MapScope& operator=(const MapScope&) = delete;

    /// Take ownership of the scene's renderers once the load has created
    /// them. The first map entry builds them; later entries find them there.
    void adoptRenderers();

    [[nodiscard]] uint32_t mapId() const { return mapId_; }
    [[nodiscard]] JobGroup& jobs() { return jobs_; }

    /// The local player's model in this scene. It is one of the scene's
    /// instances, so it ends with the scene and each map spawns it anew.
    struct PlayerModel {
        bool spawned = false;
        uint64_t guid = 0;
        uint32_t appearanceBytes = 0;
        uint8_t facialFeatures = 0;
    };
    [[nodiscard]] PlayerModel& playerModel() { return playerModel_; }
    [[nodiscard]] const PlayerModel& playerModel() const { return playerModel_; }

    /// Where the player stood on a transport of this map last frame, so the
    /// walk across its deck is added to the fixed ride offset instead of
    /// locking movement or recomputing the offset from an absolute position
    /// (see application.cpp's "M2 transport riding" block).
    struct TransportRide {
        glm::vec3 m2LockedCanonical{0.0f};
        bool hasM2Lock = false;
        glm::vec3 wmoLockedRender{0.0f};
        bool hasWmoLock = false;
        uint64_t wmoTransportGuid = 0;
        /// Boarded or transferred onto a WMO ship whose deck collision is not
        /// loaded yet: the rider keeps the boarding offset (and the camera
        /// follow stays frozen) until this transport's floor exists, rather
        /// than gravity folding into the attachment and dropping the rider
        /// through the hull.
        bool deckFloorPending = false;
    };
    [[nodiscard]] TransportRide& ride() { return ride_; }

private:
    Services services_;
    uint32_t mapId_;
    // Declaration order is the reverse of release: the jobs outlive the state
    // their workers write, the spawn state outlives nothing that reads it.
    JobGroup jobs_;
    EntitySpawner::SpawnState spawns_{jobs_};
    std::unique_ptr<rendering::MapRenderSession> render_;
    std::unique_ptr<WorldLoader::Session> load_;
    PlayerModel playerModel_;
    TransportRide ride_;
};

} // namespace core
} // namespace wowee
