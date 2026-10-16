#include "core/application.hpp"
#include "core/map_scope.hpp"
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
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
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


// The presentation's part of a map's load. The order of the load - the
// session, the map scope, the camera, the state - is ClientLoop's; these are
// called from it at their points.

void Application::mapEntryBegun(const WorldLoader::Entry& entry) {
    const auto [mapId, x, y, z] = entry;
    if (renderer) renderer->getCharacterPosition() = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(x, y, z)));
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
}

void Application::mapRenderersReady(const WorldLoader::Entry& entry, const std::string& mapName) {
    onWorldRenderersReady();
    setActiveMapName(mapName);
    if (audioCoordinator_) audioCoordinator_->loadWorldSounds(assetManager.get(), zoneTracker_ ? zoneTracker_->getZoneManager() : nullptr);
    const glm::vec3 spawnRender = coords::canonicalToRender(coords::serverToCanonical(glm::vec3(entry.x, entry.y, entry.z)));
    // Build faction hostility map for this character's race
    if (gameHandler) {
        const game::Character* activeChar = gameHandler->getActiveCharacter();
        if (activeChar) {
            buildFactionHostilityMap(static_cast<uint8_t>(activeChar->race));
        }
    }

    // The player's model is an instance of this map's scene, so every map
    // spawns it; the previous one ended with the previous scope.
    if (gameHandler) {
        const game::Character* activeChar = gameHandler->getActiveCharacter();
        if (activeChar) {
            if (!currentMap()->playerModel().spawned) {
                playerRace_ = activeChar->race;
                playerGender_ = activeChar->gender;
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
}

void Application::placeOnInstanceFloor(const WorldLoader::Entry& entry) {
    if (auto* wmo = renderer->getWMORenderer()) {
        auto position = renderer->getCharacterPosition();
        if (auto floor = wmo->getFloorHeight(position.x, position.y, position.z + 5)) {
            position.z = *floor + 0.1f;
            renderer->getCharacterPosition() = position;
            if (gameHandler) {
                const auto canonical = coords::renderToCanonical(position);
                gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
            }
        } else LOG_WARNING("Instance spawn floor unavailable on map ", entry.mapId,
                           "; retaining server spawn height until warmup");
    }
}

void Application::mapGeometryReady(const WorldLoader::Entry& entry) {
    renderer->setCharacterYaw(gameHandler ? coords::canonicalToCharacterYawDeg(gameHandler->getMovementInfo().orientation) : 0);
    spawnInstancePortalVisuals(entry.mapId, renderer.get(), assetManager.get());
}

} // namespace wowee::core
