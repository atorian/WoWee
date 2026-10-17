#include "core/zone_tracker.hpp"

#include "audio/audio_coordinator.hpp"
#include "game/game_handler.hpp"
#include "game/zone_manager.hpp"
#include "rendering/terrain_manager.hpp"

namespace wowee {
namespace core {

ZoneTracker::ZoneTracker() : zoneManager_(std::make_unique<game::ZoneManager>()) {
    zoneManager_->initialize();
}

ZoneTracker::~ZoneTracker() = default;

void ZoneTracker::loadZoneTable(pipeline::AssetManager* assets) {
    if (assets) zoneManager_->enrichFromDBC(assets);
}

uint32_t ZoneTracker::resolveAreaZoneId(uint32_t areaId) const {
    return zoneManager_->resolveAreaZoneId(areaId);
}

void ZoneTracker::update(const rendering::TerrainManager* terrain, const glm::vec3& playerPosition,
                         const game::GameHandler* game, const audio::AudioCoordinator* audio) {
    // A zone remembered from the last map is worse than none: the sticky
    // answer below would hold Duskwood's pinned midnight over a continent
    // away until the first chunk of the new one loaded.
    if (game) {
        const uint32_t mapId = game->getCurrentMapId();
        if (mapId != lastResolvedZoneMapId_) {
            lastResolvedZoneMapId_ = mapId;
            lastResolvedZoneId_ = 0;
            areaId_ = 0;
        }
    }

    onOutdoorPvpObjective_ = false;
    uint32_t tileZoneId = 0;
    if (terrain) {
        if (const auto areaId = terrain->getAreaIdAt(playerPosition.x, playerPosition.y)) {
            onOutdoorPvpObjective_ = zoneManager_->isOutdoorPvpArea(*areaId);
            areaId_ = *areaId;
            lastResolvedZoneId_ = zoneManager_->resolveAreaZoneId(*areaId);
            zoneId_ = lastResolvedZoneId_;
            return;
        }
        const auto tile = terrain->getCurrentTile();
        tileZoneId = zoneManager_->getZoneId(tile.x, tile.y);
    }

    // The chunk under the player did not say which area it is - either its
    // ADT is not resident yet, or its area id is zero, which a great many
    // chunks carry. That is "this chunk does not know", not "the zone
    // changed", and answering from a different source instead made the zone
    // id flip back and forth as the player walked from a chunk that knew to
    // one that did not.
    //
    // Nothing about that is quiet. The zone id picks the dark-zone ambience
    // override, which replaces the four sky colours outright, and in Duskwood
    // it also pins the visual hour to the small hours - so the sky changed
    // brightness every time a chunk boundary was crossed and held still the
    // moment the player did. Keep the last chunk that did know.
    if (lastResolvedZoneId_ != 0) {
        zoneId_ = lastResolvedZoneId_;
    } else if (game && game->getWorldStateZoneId() != 0) {
        zoneId_ = zoneManager_->resolveAreaZoneId(game->getWorldStateZoneId());
    } else if (audio && audio->getCurrentZoneId() != 0) {
        zoneId_ = audio->getCurrentZoneId();
    } else {
        zoneId_ = tileZoneId;
    }
}

} // namespace core
} // namespace wowee
