#include "core/map_scope.hpp"
#include "core/logger.hpp"
#include "core/player_view.hpp"
#include "game/entity_controller.hpp"
#include "game/game_handler.hpp"
#include "game/transport_manager.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/map_render_session.hpp"
#include <stdexcept>

namespace wowee::core {

MapScope::MapScope(const Services& services, uint32_t mapId)
    : services_(services), mapId_(mapId) {
    if (services_.spawner.hasMap())
        throw std::logic_error("EntitySpawner is already bound to another map scope");
    if (services_.renderer)
        render_ = std::make_unique<rendering::MapRenderSession>(*services_.renderer, jobs_.token());
    services_.spawner.bindMap(&spawns_);
    LOG_INFO("Map scope started: map=", mapId_);
    // The server does not send an object that is still in range a second
    // time, and a create that arrived before this scope existed - in the same
    // frame as the login, or kept across a far teleport - had no scene to go
    // to. The entities are the authority; present them once, here.
    services_.game.getEntityController()->announceObjectsInView();
}

MapScope::MapScope(const Services& services, WorldLoader& loader, WorldLoader::Entry entry)
    : MapScope(services, entry.mapId) {
    load_ = loader.begin(entry, jobs_);
}

void MapScope::adoptRenderers() {
    if (render_) render_->attach();
}

MapScope::~MapScope() {
    LOG_INFO("Map scope ending: map=", mapId_);
    // Nothing of this map is current any more: every queued request, finished
    // result and worker step checks this group's token.
    jobs_.cancel();
    services_.spawner.bindMap(nullptr);

    // Join what still runs against this map before anything it writes goes:
    // the map preparation, the terrain workers, the pooled spawn jobs.
    load_.reset();
    if (render_) render_->stopStreaming();
    jobs_.wait();

    // What refers to this scene's instances and terrain lets go of them first.
    services_.playerView.setCharacterFollow(0);
    services_.playerView.detachWorld();
    if (auto* animation = services_.playerView.getAnimationController()) animation->clearMount();
    // Transport GUIDs are reused across maps (an Undercity elevator's GUID
    // becomes a Deeprun tram's), and each transport is bound to an instance
    // of this scene, so the logical transports end with it.
    services_.game.clearPlayerTransport();
    if (auto* transports = services_.game.getTransportManager()) transports->clearTransports();
    // The equipment is composited onto the player model, which is one of this
    // scene's instances; the next scene's model is dressed from scratch.
    services_.game.resetEquipmentDirtyTracking();

    // Instances retire behind the frame fences; the renderers' model and
    // texture caches stay for the next map.
    render_.reset();
}

} // namespace wowee::core
