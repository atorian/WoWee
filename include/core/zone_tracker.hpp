#pragma once

#include <cstdint>
#include <memory>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game { class GameHandler; class ZoneManager; }
namespace audio { class AudioCoordinator; }
namespace rendering { class TerrainManager; }

namespace core {

/// Which zone the player is in, for everything that changes with it: the
/// light and weather the renderer draws, the music, the minimap's label and
/// the addons' zone queries.
///
/// Resolved once a frame from the area the streamed terrain has under the
/// player, and from the realm when the terrain says nothing. Owned by the
/// Application, which hands the answer to the renderer - the renderer knows
/// terrain, not zones.
class ZoneTracker {
public:
    ZoneTracker();
    ~ZoneTracker();

    /// The zone table, music included, from the client's DBCs.
    void loadZoneTable(pipeline::AssetManager* assets);

    /// Resolve the zone for this frame. Any of the pointers may be null.
    void update(const rendering::TerrainManager* terrain, const glm::vec3& playerPosition,
                const game::GameHandler* game, const audio::AudioCoordinator* audio);

    /// The zone the player is in; 0 when nothing knows.
    [[nodiscard]] uint32_t zoneId() const { return zoneId_; }
    /// The area under the player is a world PvP objective. Asked of the area
    /// rather than its zone: the flag is on the subzone.
    [[nodiscard]] bool isOnOutdoorPvpObjective() const { return onOutdoorPvpObjective_; }
    /// The zone an area id belongs to, by AreaTable's parentage.
    [[nodiscard]] uint32_t resolveAreaZoneId(uint32_t areaId) const;

    game::ZoneManager* getZoneManager() { return zoneManager_.get(); }

private:
    std::unique_ptr<game::ZoneManager> zoneManager_;
    uint32_t zoneId_ = 0;
    bool onOutdoorPvpObjective_ = false;
    /// The last zone a terrain chunk actually named. Many chunks carry an area
    /// id of zero, and a tile being loaded carries none at all, so the lookup
    /// answers "do not know" often - and answering from somewhere else instead
    /// made the zone flip at chunk boundaries while walking.
    uint32_t lastResolvedZoneId_ = 0;
    /// The map that answer belongs to, so it is dropped on a
    /// continent change rather than held across one.
    uint32_t lastResolvedZoneMapId_ = 0xFFFFFFFFu;
};

} // namespace core
} // namespace wowee
