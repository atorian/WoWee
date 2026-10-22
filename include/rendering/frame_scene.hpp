#pragma once
#include <optional>
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/footprint_renderer.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/grass_renderer.hpp"
#include "rendering/mount_dust.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/lightning.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/precipitation.hpp"


#include "rendering/rt_scene.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/distant_terrain.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/offscreen_composite.hpp"
#include "rendering/minimap.hpp"

namespace wowee::rendering {

/// Owned world data extracted on the game side of a frame.
struct FrameScene {
    uint64_t sequence = 0;
    std::optional<SceneView> reflectionView;
    std::shared_ptr<SnapshotLease> resources;
    RtScene::Changes rtChanges;
    SkySystem::Frame sky;
    QuestMarkerRenderer::Frame markers;
    FootprintRenderer::Frame footprints;
    std::vector<float> dust;
    SwimEffects::Frame swim;
    Lightning::Frame lightning;
    ChargeEffect::Frame charge;
    Precipitation::Frame precipitation;
    TerrainRenderer::DrawList terrain;
    DistantTerrain::Frame distantTerrain;
    WaterRenderer::DrawList water;
    GrassRenderer::Frame grass;
    std::shared_ptr<const M2Renderer::Frame> models, skyModels;
    std::shared_ptr<const WMORenderer::Frame> buildings;
    std::shared_ptr<const CharacterRenderer::Frame> characters;
    std::vector<OffscreenComposite::Request> composites;
    Minimap::Frame minimap;
};

} // namespace wowee::rendering
