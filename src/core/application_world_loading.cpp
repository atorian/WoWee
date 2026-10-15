#include "core/application.hpp"
#include "core/frame_phase.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "core/player_view.hpp"
#include "core/world_entry_callback_handler.hpp"
#include "core/zone_tracker.hpp"
#include "audio/audio_coordinator.hpp"
#include "game/game_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/transport_manager.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/footprint_renderer.hpp"
#include "rendering/loading_screen.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include <glm/gtc/quaternion.hpp>

namespace wowee::core {
namespace {

struct InstancePortalVisual {
    uint32_t mapId;
    glm::vec3 canonicalPos;
    float canonicalYaw;
    float scale;
};

void spawnInstancePortalVisuals(uint32_t mapId,
                                rendering::Renderer* renderer,
                                pipeline::AssetManager* assetManager) {
    if (!renderer || !assetManager || !assetManager->isInitialized()) return;
    auto* m2Renderer = renderer->getM2Renderer();
    if (!m2Renderer) return;

    static constexpr const char* kPortalPath =
        "World\\GENERIC\\ACTIVEDOODADS\\INSTANCEPORTAL\\InstancePortal.m2";
    static constexpr const char* kPortalModelName =
        "World\\GENERIC\\ACTIVEDOODADS\\INSTANCEPORTAL\\InstancePortal.m2#world-entry-glow";
    static const uint32_t kPortalModelId =
        static_cast<uint32_t>(std::hash<std::string>{}(kPortalModelName));

    if (!m2Renderer->hasModel(kPortalModelId)) {
        auto m2Data = assetManager->readFile(kPortalPath);
        if (m2Data.empty()) {
            LOG_WARNING("Instance portal visual unavailable: ", kPortalPath);
            return;
        }

        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        model.name = kPortalModelName;

        const std::string skinPath = pipeline::skinPathForM2(std::string(kPortalPath));
        auto skinData = assetManager->readFile(skinPath);
        if (!skinData.empty() && model.version >= 264) {
            pipeline::M2Loader::loadSkin(skinData, model);
        }

        if (!m2Renderer->loadModel(model, kPortalModelId)) {
            LOG_WARNING("Failed to load instance portal visual model: ", kPortalPath);
            return;
        }
        m2Renderer->markModelAsSpellEffect(kPortalModelId);
    }

    static const InstancePortalVisual kPortals[] = {
        // Ironforge station -> Deeprun Tram
        {.mapId = 0,   .canonicalPos = glm::vec3(-1330.46f, -4840.26f, 503.85f),
         .canonicalYaw = 3.10f, .scale = 4.25f},
        // Deeprun Tram -> Ironforge station
        {.mapId = 369, .canonicalPos = glm::vec3(10.50f, 76.03f, -2.30f),
         .canonicalYaw = 3.12f, .scale = 4.25f},
        // Stormwind station -> Deeprun Tram. Coordinates from a live playthrough's
        // actual AreaTrigger 2173 fire: entrance trigger position on map 0, and the
        // canonical position the player actually landed at on map 369 after transfer
        // (Z lifted by the same ~+2.0 offset the Ironforge map-369 entry uses relative
        // to its own arrival point, since both tunnel ends have similar floor height).
        // Yaw reuses the Ironforge value as a starting point - Stormwind's entrance
        // faces a different direction, so this likely needs live tuning.
        {.mapId = 0,   .canonicalPos = glm::vec3(514.03f, -8346.46f, 97.60f),
         .canonicalYaw = 3.12f, .scale = 4.25f},
        // Deeprun Tram -> Stormwind station
        {.mapId = 369, .canonicalPos = glm::vec3(2490.91f, 68.30f, -2.30f),
         .canonicalYaw = 3.12f, .scale = 4.25f},
        // The Stockade exit. The WMO contains this InstancePortal doodad, but
        // WMO-child matrix instances bypass the standalone portal presentation.
        // Position and scale come from StormwindJail.wmo's authored doodad after
        // applying the instance's 180-degree placement rotation.
        {.mapId = 34,  .canonicalPos = glm::vec3(0.6882f, 45.7492f, -16.1133f),
         .canonicalYaw = 0.0f, .scale = 1.36f},
    };

    for (const auto& portal : kPortals) {
        if (portal.mapId != mapId) continue;

        glm::vec3 renderPos = core::coords::canonicalToRender(portal.canonicalPos);
        float renderYaw = portal.canonicalYaw + glm::radians(90.0f);
        // Each entry keeps the scale appropriate to its authored doorway; the
        // Deeprun entries are intentionally larger because their original effect
        // was too subtle at station scale.
        uint32_t instanceId = m2Renderer->createInstance(
            kPortalModelId, renderPos, glm::vec3(0.0f, 0.0f, renderYaw), portal.scale);
        if (instanceId) {
            m2Renderer->setSkipCollision(instanceId, true);
            LOG_INFO("Spawned instance portal visual map=", mapId,
                     " canonical=(", portal.canonicalPos.x, ", ",
                     portal.canonicalPos.y, ", ", portal.canonicalPos.z, ")");
        }
    }
}

} // namespace


void Application::beginWorldLoading(WorldLoader::Entry entry) {
    if (!requireTransitions("enter loading world")) return;
    const auto [mapId, x, y, z] = entry;
    worldLoader_->cancel();
    // --- Clean up previous map's state on map change ---
    // (Same cleanup as logout, but preserves player identity and renderer objects.)
    LOG_DEBUG("Begin world load: map=", mapId, " previousMap=", worldLoader_->getLoadedMapId());
    bool hasRendererData = renderer && (renderer->getWMORenderer() || renderer->getM2Renderer());
    if (worldLoader_->getLoadedMapId() != 0xFFFFFFFF || hasRendererData) {
        LOG_WARNING("Map change: cleaning up old map ", worldLoader_->getLoadedMapId(), " before loading map ", mapId);

        // Clear pending queues first (these don't touch GPU resources)
        entitySpawner_->clearAllQueues();

        // Transport GUIDs are reused across maps (for example, Undercity elevator
        // GUIDs become Deeprun tram GUIDs). The renderer instances are map-local,
        // so the logical transport/path state must be map-local too; otherwise a
        // newly spawned SubwayCar is rebound to the old elevator path.
        if (gameHandler) {
            gameHandler->clearPlayerTransport();
            if (auto* tm = gameHandler->getTransportManager()) {
                tm->clearTransports();
            }
        }

        if (renderer) {
            // Clear all world geometry from old map (including textures/models).
            // Old GPU handles retire only after all frame fences; CPU scene
            // ownership can switch immediately without stalling the device.
            if (auto* wmo = renderer->getWMORenderer()) {
                wmo->clearAll();
            }
            if (auto* m2 = renderer->getM2Renderer()) {
                m2->clear();
            }

            // Full clear of character renderer: removes all instances, models,
            // textures, retaining pools until their retired sets are freed. This prevents stale GPU
            // resources from accumulating across map changes (old creature
            // models, bone buffers, texture descriptor sets) which can cause
            // VK_ERROR_DEVICE_LOST on some drivers.
            if (auto* cr = renderer->getCharacterRenderer()) {
                cr->clear();
                getPlayerView()->setCharacterFollow(0);
            }
            // Reset equipment dirty tracking so composited textures are rebuilt
            // after spawnPlayerCharacter() recreates the character instance.
            if (gameHandler) {
                gameHandler->resetEquipmentDirtyTracking();
            }

            if (auto* terrain = renderer->getTerrainManager()) {
                terrain->softReset();
                terrain->setStreamingEnabled(false); // Activate only after the new map name is installed.
            }
            if (auto* questMarkers = renderer->getQuestMarkerRenderer()) {
                questMarkers->clear();
            }
            if (auto* footprints = renderer->getFootprintRenderer()) {
                footprints->clear();
            }
            if (auto* ac = getAnimationController()) ac->clearMount();
        }

        // Clear application-level instance tracking (after renderer cleanup)
        entitySpawner_->resetAllState();

        // Force player character re-spawn on new map
        playerCharacterSpawned = false;
    }


    if (gameHandler) {
        gameHandler->cancelFollow();
        gameHandler->getMovementHandler()->stopInputMovement();
    }
    if (auto* camera = getCameraController()) {
        const auto spawn = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(x, y, z)));
        const auto yaw = gameHandler ? coords::canonicalToCharacterYawDeg(gameHandler->getMovementInfo().orientation) : 0;
        camera->clearMovementInputs();
        camera->releaseMouseCapture();
        camera->setOnlineMode(true);
        camera->setDefaultSpawn(spawn, yaw, -15.0f);
        camera->reset();
        camera->setEnabled(false);
    }
    if (renderer) renderer->getCharacterPosition() = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(x, y, z)));
    if (worldEntryCallbacks_) {
        worldEntryCallbacks_->setWorldEntryMovementGraceTimer(2.0f);
        worldEntryCallbacks_->setTaxiLandingClampTimer(0);
        worldEntryCallbacks_->setLastTaxiFlight(false);
    }
    setState(AppState::LOADING_WORLD);
    if (!loadingScreen_) {
        loadingScreen_ = std::make_unique<rendering::LoadingScreen>();
        loadingScreen_->setVkContext(window->getVkContext());
        loadingScreen_->setSDLWindow(window->getSDLWindow());
        if (!loadingScreen_->initialize()) LOG_ERROR("Loading background unavailable; showing progress without its image");
    }
    const char* friendly = WorldLoader::mapDisplayName(mapId);
    loadingScreen_->setZoneName(friendly ? friendly : (gameHandler ? gameHandler->getMapName(mapId) : std::string()));
    loadingScreen_->setStatus("Preparing world...");
    loadingScreen_->setProgress(0);
    worldLoader_->begin(entry);
}

void Application::prepareWorldPlayer() {
    if (!requireTransitions("prepare loading player")) return;
    onWorldRenderersReady();
    setActiveMapName(worldLoader_->mapName());
    if (audioCoordinator_) audioCoordinator_->loadWorldSounds(assetManager.get(), zoneTracker_ ? zoneTracker_->getZoneManager() : nullptr);
    const auto entry = worldLoader_->entry();
    const glm::vec3 spawnRender = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(entry.x, entry.y, entry.z)));
    // Build faction hostility map for this character's race
    if (gameHandler) {
        const game::Character* activeChar = gameHandler->getActiveCharacter();
        if (activeChar) {
            buildFactionHostilityMap(static_cast<uint8_t>(activeChar->race));
        }
    }

    // Spawn player model for online mode (skip if already spawned, e.g. teleport)
    if (gameHandler) {
        const game::Character* activeChar = gameHandler->getActiveCharacter();
        if (activeChar) {
            const uint64_t activeGuid = gameHandler->getActiveCharacterGuid();
            const bool appearanceChanged =
                (activeGuid != spawnedPlayerGuid_) ||
                (activeChar->appearanceBytes != spawnedAppearanceBytes_) ||
                (activeChar->facialFeatures != spawnedFacialFeatures_) ||
                (activeChar->race != playerRace_) ||
                (activeChar->gender != playerGender_) ||
                (activeChar->characterClass != playerClass_);

            if (!playerCharacterSpawned || appearanceChanged) {
                if (appearanceChanged) {
                    LOG_INFO("Respawning player model for new/changed character: guid=0x",
                             std::hex, activeGuid, std::dec);
                }
                // Remove old instance so we don't keep stale visuals.
                if (renderer && renderer->getCharacterRenderer()) {
                    uint32_t oldInst = renderer->getCharacterInstanceId();
                    if (oldInst > 0) {
                        getPlayerView()->setCharacterFollow(0);
                        if (auto* ac = getAnimationController()) ac->clearMount();
                        renderer->getCharacterRenderer()->removeInstance(oldInst);
                    }
                }
                playerCharacterSpawned = false;
                spawnedPlayerGuid_ = 0;
                spawnedAppearanceBytes_ = 0;
                spawnedFacialFeatures_ = 0;

                playerRace_ = activeChar->race;
                playerGender_ = activeChar->gender;
                playerClass_ = activeChar->characterClass;
                spawnSnapToGround = false;
                if (appearanceComposer_) appearanceComposer_->setWeaponsSheathed(false);
                if (appearanceComposer_) appearanceComposer_->loadEquippedWeapons(); // will no-op until instance exists
                spawnPlayerCharacter();
            }
            renderer->getCharacterPosition() = spawnRender;
            LOG_INFO("Online player at render pos (", spawnRender.x, ", ", spawnRender.y, ", ", spawnRender.z, ")");
        } else {
            LOG_WARNING("No active character found for player model spawning");
        }
    }


    // The renderer may have created the camera's world dependencies just now.
    if (auto* camera = getCameraController()) {
        const float yaw = gameHandler ? coords::canonicalToCharacterYawDeg(gameHandler->getMovementInfo().orientation) : 0;
        camera->setOnlineMode(true);
        camera->setDefaultSpawn(spawnRender, yaw, -15.0f);
        camera->reset();
        camera->setEnabled(false);
    }
    if (gameHandler && renderer && renderer->getCharacterRenderer()) {
        auto* cr = renderer->getCharacterRenderer();
        auto* spawner = entitySpawner_.get();

        gameHandler->setNpcDeathCallback([cr, spawner](uint64_t guid) {
            spawner->markCreatureDead(guid);
            uint32_t instanceId = spawner->getCreatureInstanceId(guid);
            if (instanceId == 0) instanceId = spawner->getPlayerInstanceId(guid);
            if (instanceId != 0 && cr) {
                cr->playAnimation(instanceId, rendering::anim::DEATH, false);
            }
        });

        gameHandler->setNpcRespawnCallback([cr, spawner](uint64_t guid) {
            spawner->unmarkCreatureDead(guid);
            uint32_t instanceId = spawner->getCreatureInstanceId(guid);
            if (instanceId == 0) instanceId = spawner->getPlayerInstanceId(guid);
            if (instanceId != 0 && cr) {
                cr->playAnimation(instanceId, rendering::anim::STAND, true);
            }
        });

        // Probe the creature model for the best available attack animation
        gameHandler->setNpcSwingCallback([cr, spawner](uint64_t guid) {
            uint32_t instanceId = spawner->getCreatureInstanceId(guid);
            if (instanceId == 0) instanceId = spawner->getPlayerInstanceId(guid);
            if (instanceId != 0 && cr) {
                const auto attackAnims =
                rendering::anim::meleeAnimChain(rendering::anim::MeleeChain::Generic);
                bool played = false;
                for (uint32_t anim : attackAnims) {
                    if (cr->hasAnimation(instanceId, anim)) {
                        cr->playAnimation(instanceId, anim, false);
                        played = true;
                        break;
                    }
                }
                if (!played) cr->playAnimation(instanceId, rendering::anim::ATTACK_UNARMED, false);
            }
        });
    }


}

void Application::finishWorldGeometry() {
    if (!requireTransitions("finish loading geometry")) return;
    if (worldLoader_->isInstance()) {
        if (auto* wmo = renderer->getWMORenderer()) {
            auto position = renderer->getCharacterPosition();
            if (auto floor = wmo->getFloorHeight(position.x, position.y, position.z + 5)) {
                position.z = *floor + 0.1f;
                renderer->getCharacterPosition() = position;
                if (gameHandler) {
                    const auto canonical = coords::renderToCanonical(position);
                    gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
                }
            } else LOG_WARNING("Instance spawn floor unavailable on map ", worldLoader_->entry().mapId,
                               "; retaining server spawn height until warmup");
        }
    }
    if (auto* camera = getCameraController()) camera->reset();
    renderer->setCharacterYaw(gameHandler ? coords::canonicalToCharacterYawDeg(gameHandler->getMovementInfo().orientation) : 0);
    spawnInstancePortalVisuals(worldLoader_->entry().mapId, renderer.get(), assetManager.get());
    worldLoader_->beginWarmup();
}

void Application::completeWorldLoading() {
    if (!requireTransitions("complete loading world")) return;
    if (!worldLoader_->finish()) return;
    if (auto* camera = getCameraController()) {
        camera->clearMovementInputs();
        camera->suppressMovementFor(0);
        // The old warmup advanced this ten-second entry grace with its frames.
        camera->suspendGravityFor(std::max(0.0f, 10.0f - worldLoader_->warmupSeconds()));
        camera->setEnabled(true);
        camera->startIntroPan(2.8f, 140.0f);
    }
    setState(AppState::IN_GAME);
    worldInterfaceEntryPending_ = true;
}

} // namespace wowee::core
