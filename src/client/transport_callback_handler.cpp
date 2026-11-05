#include "client/transport_callback_handler.hpp"
#include "game/game_events.hpp"
#include "client/player_view.hpp"
#include "client/appearance_composer.hpp"
#include "client/entity_spawner.hpp"
#include "client/world_loader.hpp"
#include "pipeline/coordinates.hpp"
#include "foundation/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/m2_renderer.hpp"
#include "game/game_handler.hpp"
#include "game/transport_manager.hpp"

#include <set>

namespace wowee { namespace core {

TransportCallbackHandler::TransportCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer& renderer,
    PlayerView& playerView,
    game::GameHandler& gameHandler,
    AppearanceComposer* appearanceComposer)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , playerView_(playerView)
    , gameHandler_(gameHandler)
    , appearanceComposer_(appearanceComposer)
{
}

void TransportCallbackHandler::setupCallbacks() {
    // Mount callback (online mode) - defer heavy model load to next frame
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::PlayerMountChanged>(
        [this](const game::events::PlayerMountChanged& e) {
            if (e.mountDisplayId == 0) {
                // Dismount is instant (no loading needed)
                if (renderer_.getCharacterRenderer() && entitySpawner_.getMountInstanceId() != 0) {
                    renderer_.scene().characters().removeInstance(entitySpawner_.getMountInstanceId());
                    entitySpawner_.clearMountState();
                }
                entitySpawner_.setMountDisplayId(0);
                if (auto* ac = playerView_.getAnimationController()) ac->clearMount();
                LOG_INFO("Dismounted");
                return;
            }
            // Queue the mount for processing in the next update() frame
            entitySpawner_.setMountDisplayId(e.mountDisplayId);

            // Mounting stows drawn weapons, matching the original client.
            if (appearanceComposer_ && !appearanceComposer_->isWeaponsSheathed()) {
                appearanceComposer_->setWeaponsSheathed(true);
                appearanceComposer_->loadEquippedWeapons();
            }
        }));

    // Taxi precache callback - preload terrain tiles along flight path
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::TaxiPathPrecacheRequested>(
        [this](const game::events::TaxiPathPrecacheRequested& e) {
            if (!renderer_.getTerrainManager()) return;

            std::set<std::pair<int, int>> uniqueTiles;

            // Sample waypoints along path and gather tiles.
            // Denser sampling + neighbor coverage reduces in-flight stream spikes.
            const size_t stride = 2;
            for (size_t i = 0; i < e.path.size(); i += stride) {
                const auto& waypoint = e.path[i];
                glm::vec3 renderPos = core::coords::canonicalToRender(waypoint);
                int tileX = static_cast<int>(32 - (renderPos.x / core::coords::TILE_SIZE));
                int tileY = static_cast<int>(32 - (renderPos.y / core::coords::TILE_SIZE));

                if (tileX >= 0 && tileX <= 63 && tileY >= 0 && tileY <= 63) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        for (int dy = -1; dy <= 1; ++dy) {
                            int nx = tileX + dx;
                            int ny = tileY + dy;
                            if (nx >= 0 && nx <= 63 && ny >= 0 && ny <= 63) {
                                uniqueTiles.insert({nx, ny});
                            }
                        }
                    }
                }
            }
            // Ensure final destination tile is included.
            if (!e.path.empty()) {
                glm::vec3 renderPos = core::coords::canonicalToRender(e.path.back());
                int tileX = static_cast<int>(32 - (renderPos.x / core::coords::TILE_SIZE));
                int tileY = static_cast<int>(32 - (renderPos.y / core::coords::TILE_SIZE));
                if (tileX >= 0 && tileX <= 63 && tileY >= 0 && tileY <= 63) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        for (int dy = -1; dy <= 1; ++dy) {
                            int nx = tileX + dx;
                            int ny = tileY + dy;
                            if (nx >= 0 && nx <= 63 && ny >= 0 && ny <= 63) {
                                uniqueTiles.insert({nx, ny});
                            }
                        }
                    }
                }
            }

            std::vector<std::pair<int, int>> tilesToLoad(uniqueTiles.begin(), uniqueTiles.end());
            if (tilesToLoad.size() > 512) {
                tilesToLoad.resize(512);
            }
            LOG_INFO("Precaching ", tilesToLoad.size(), " tiles for taxi route");
            renderer_.getTerrainManager()->precacheTiles(tilesToLoad);
        }));

    // Taxi orientation callback - update mount rotation during flight
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::TaxiOrientationChanged>(
        [this](const game::events::TaxiOrientationChanged& e) {
            if (playerView_.getCameraController()) {
                // Taxi callback now provides render-space yaw directly.
                float yawDegrees = glm::degrees(e.yaw);
                playerView_.getCameraController()->setFacingYaw(yawDegrees);
                renderer_.setCharacterYaw(yawDegrees);
                // Set mount pitch and roll for realistic flight animation
                if (auto* ac = playerView_.getAnimationController()) ac->setMountPitchRoll(e.pitch, e.roll);
            }
        }));

    // Taxi landing position correction callback - see its declaration comment in
    // game_handler.hpp for why this exists (Application's own per-frame render
    // sync stops as soon as onTaxi goes false, which happens before this fires).
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::PlayerPositionCorrected>(
        [this](const game::events::PlayerPositionCorrected& e) {
            glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(e.x, e.y, e.z));
            renderer_.getCharacterPosition() = renderPos;
            if (playerView_.getCameraController()) {
                glm::vec3* followTarget = playerView_.getCameraController()->getFollowTargetMutable();
                if (followTarget) {
                    *followTarget = renderPos;
                }
            }
        }));

    // Taxi flight start callback - keep non-blocking to avoid hitching at takeoff.
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::TaxiFlightStarted>(
        [this](const game::events::TaxiFlightStarted&) {
            if (renderer_.getTerrainManager() && renderer_.getM2Renderer()) {
                LOG_INFO("Taxi flight start: incremental terrain/M2 streaming active");
                uint32_t m2Count = renderer_.getM2Renderer()->getModelCount();
                uint32_t instCount = renderer_.getM2Renderer()->getInstanceCount();
                LOG_INFO("Current M2 VRAM state: ", m2Count, " models (", instCount, " instances)");
            }
        }));

    // Transport spawn callback (online mode) - register transports with TransportManager
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::TransportSpawned>(
        [this](const game::events::TransportSpawned& e) {
            // Get the GameObject instance now so late queue processing can rely on stable IDs.
            if (!entitySpawner_.hasMap()) {
                LOG_WARNING("Transport spawn callback: no map scope for GUID 0x", std::hex, e.guid, std::dec,
                            "; it is registered when the scope presents its objects");
                return;
            }
            auto& goInstances2 = entitySpawner_.getGameObjectInstances();
            auto it = goInstances2.find(e.guid);
            if (it == goInstances2.end()) {
                LOG_WARNING("Transport spawn callback: GameObject instance not found for GUID 0x", std::hex, e.guid, std::dec);
                return;
            }

            auto pendingIt = entitySpawner_.hasTransportRegistrationPending(e.guid);
            if (pendingIt) {
                entitySpawner_.updateTransportRegistration(e.guid, e.displayId, e.x, e.y, e.z, e.orientation);
            } else {
                entitySpawner_.queueTransportRegistration(e.guid, e.entry, e.displayId, e.x, e.y, e.z, e.orientation);
            }
        }));

    // Transport move callback (online mode) - update transport gameobject positions
    subscriptions_.push_back(gameHandler_.eventBus().subscribe<game::events::TransportMoved>(
        [this](const game::events::TransportMoved& e) {
            LOG_DEBUG("Transport move callback: GUID=0x", std::hex, e.guid, std::dec,
                     " pos=(", e.x, ", ", e.y, ", ", e.z, ") orientation=", e.orientation);

            auto* transportManager = gameHandler_.getTransportManager();
            if (!transportManager) {
                LOG_WARNING("Transport move callback: TransportManager is null!");
                return;
            }
            // No scene yet: the map scope presents the transport from its entity,
            // at the position this update has just written there.
            if (!entitySpawner_.hasMap()) return;

            if (entitySpawner_.hasTransportRegistrationPending(e.guid)) {
                entitySpawner_.setTransportPendingMove(e.guid, e.x, e.y, e.z, e.orientation);
                LOG_DEBUG("Queued transport move for pending registration GUID=0x", std::hex, e.guid, std::dec);
                return;
            }

            // Check if transport exists - if not, treat this as a late spawn (reconnection/server restart)
            if (!transportManager->getTransport(e.guid)) {
                LOG_DEBUG("Received position update for unregistered transport 0x", std::hex, e.guid, std::dec,
                         " - auto-spawning from position update");

                // Get transport info from entity manager
                auto entity = gameHandler_.getEntityManager().getEntity(e.guid);
                if (entity && entity->getType() == game::ObjectType::GAMEOBJECT) {
                    auto go = std::static_pointer_cast<game::GameObject>(entity);
                    uint32_t entry = go->getEntry();
                    uint32_t displayId = go->getDisplayId();

                    // Find the WMO instance for this transport (should exist from earlier GameObject spawn)
                    auto& goInstances3 = entitySpawner_.getGameObjectInstances();
                    auto it = goInstances3.find(e.guid);
                    if (it != goInstances3.end()) {
                        uint32_t wmoInstanceId = it->second.instanceId;
                        const bool preferServerData = gameHandler_.world().hasServerTransportUpdate(e.guid);
                        // Coordinates are already canonical (converted in game_handler.cpp)
                        glm::vec3 canonicalSpawnPos(e.x, e.y, e.z);
                        const bool isM2Transport = !it->second.isWmo;
                        transportManager->resolveAndRegisterSpawn(e.guid, entry, displayId, canonicalSpawnPos,
                                                                  wmoInstanceId, isM2Transport, preferServerData,
                                                                  e.orientation);
                    } else {
                        entitySpawner_.setTransportPendingMove(e.guid, e.x, e.y, e.z, e.orientation);
                        LOG_DEBUG("Cannot auto-spawn transport 0x", std::hex, e.guid, std::dec,
                                  " - WMO instance not found yet (queued move for replay)");
                        return;
                    }
                } else {
                    entitySpawner_.setTransportPendingMove(e.guid, e.x, e.y, e.z, e.orientation);
                    LOG_DEBUG("Cannot auto-spawn transport 0x", std::hex, e.guid, std::dec,
                              " - entity not found in EntityManager (queued move for replay)");
                    return;
                }
            }

            // Update TransportManager's internal state (position, rotation, transform matrices)
            // This also updates the WMO renderer automatically
            // Coordinates are already canonical (converted in game_handler.cpp when entity was created)
            glm::vec3 canonicalPos(e.x, e.y, e.z);
            transportManager->updateServerTransport(e.guid, canonicalPos, e.orientation);

            // Move player with transport if riding it
            if (gameHandler_.movement().isOnTransport() && gameHandler_.movement().getPlayerTransportGuid() == e.guid) {
                auto* cc = playerView_.getCameraController();
                if (cc) {
                    glm::vec3* ft = cc->getFollowTargetMutable();
                    if (ft) {
                        // Get player world position from TransportManager (handles transform properly)
                        glm::vec3 offset = gameHandler_.movement().getPlayerTransportOffset();
                        glm::vec3 worldPos = transportManager->getPlayerWorldPosition(e.guid, offset);
                        *ft = worldPos;
                    }
                }
            }
        }));
}

}} // namespace wowee::core
