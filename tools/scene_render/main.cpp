// scene_render: one frame of a static scene, drawn by the client's own
// renderer and written to a PNG - no realm, no login, no interface.
//
//     scene_render <scene.json> <out.png>
//
// Links wowee_render, wowee_pipeline and wowee_base and nothing of the client,
// so a change to a shader or a renderer rebuilds the renderer and this, and a
// frame takes the few seconds it takes to load the models.
//
// The scene, every field optional but the models:
//
//   {
//     "map": "Northrend", "mapId": 571, "zoneId": 4395,
//     "hour": 12.0,                       // the realm clock; the sky and the light
//     "weather": "clear",                 // clear | fog | rain | snow | sandstorm |
//                                         //   thunderstorm | blackrain | blacksnow
//     "weatherIntensity": 0.0,            //   0-1; there from the first frame
//     "lens": true,                       // what the weather leaves on the camera's
//                                         //   lens - it gathers over the frames
//     "splashes": true,                   // rain splashing where it lands
//     "realmTime": 0.0,                   // the realm's calendar clock, seconds since
//                                         //   1970, that the wind is drawn from; it
//                                         //   runs on with the frames
//     "wind": { "direction": 37.0,        // a fixed wind instead of the zone's:
//               "speed": 2.0 },           //   degrees it blows toward (0 render +x,
//                                         //   90 render +y), yards a second; 2 is the
//                                         //   speed the sway is tuned at
//     "server": [5815.0, 629.0, 647.0],   // where it stands, as the realm and
//                                         //   .gps give positions,
//     "origin": [5815.0, 629.0, 647.0],   //   or in render coordinates
//     "terrain": false,                   // stream the map's terrain around it,
//     "terrainRadius": 1,                 //   this many tiles out (3x3)
//     "models": [
//       { "path": "World\\Azeroth\\Elwynn\\PassiveDoodads\\LampPost\\LampPost.m2",
//         "offset": [0, 0, 0],             // render axes, from the origin
//         "rotation": [0, 0, 0],           // degrees about x, y, z
//         "scale": 1.0,
//         "doodads": false,                // a WMO's doodads, placed as its
//                                          //   children the way the client fits
//                                          //   out a transport hull
//         "hidden": false,                 // placed out of the world, as a
//                                          //   transport is while its route is
//                                          //   on the other map,
//         "shownAtFrame": 10 }             //   and back in it from this frame
//     ],
//     "characters": [                     // drawn as the client draws units,
//       { "path": "Creature\\Seal\\Seal.m2", //   and what follows units follows
//         "offset": [0, 0, 0],             //   them - the water's wake - as
//         "rotation": [0, 0, 0],           //   they move at `velocity` (render
//         "scale": 1.0,                    //   yards a second) every frame
//         "velocity": [3, 0, 0] }
//     ],
//     "camera": { "offset": [-8, 0, 3],   // from the origin; or "position"
//                 "lookAt": [0, 0, 2],    // from the origin; or "yaw"/"pitch"
//                 "velocity": [0, 6, 0],  // optional: after the frames above, it
//                 "motionFrames": 10 },   //   moves this fast (render yards/s) for
//                                         //   this many more before the kept one
//     "fov": 60.0,
//     "size": [1280, 720],
//     "frames": 30,                       // drawn before the one that is kept
//     "shadowQuality": 2,                 // 0-4, as the settings slider
//     "msaa": 1,                          // samples: 1, 2, 4 or 8
//     "fxaa": false,                      // FXAA, drawn through its own target
//     "fogStrength": 0.4,                 // the settings panel's atmosphere:
//     "fogSkyBlend": 0.7,                 //   distance fog, its pull to the sky,
//     "volumetricFog": 2,                 //   volumetric quality 0-3 and density,
//     "volumetricDensity": 1.0,           //   and the sun's rays
//     "sunShafts": true,
//     "minimap": false                    // the minimap disc in the corner
//   }
//
// Without a camera it backs off along -X far enough to see the first model.
// Render axes: z is up, yaw 0 looks along +x and 90 along +y.
//
// With --timing after the paths (tools/scene_render.sh passes it when
// SCENE_RENDER_TIMING=1), every frame prints a "timing:" line: its wall time, the
// renderer's calls and their largest parts, and the GPU's time for the last
// frame read back. A frame budget of the frames after the warm-up follows -
// the same report, percentiles and parts, as the client's frame profile.
//
// Besides the picture it prints how many draw calls the world's M2 renderer
// recorded in the kept frame, which is what the scene tests registered beside
// this tool read.

#include "core/coordinates.hpp"
#include "core/frame_budget.hpp"
#include "rendering/minimap.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/render_surface.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/wmo_renderer.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace wowee;
using json = nlohmann::json;

namespace {

/// A hidden window's swapchain: what the renderer draws onto.
class SceneSurface : public rendering::RenderSurface {
public:
    SceneSurface(SDL_Window* window, int width, int height) : width_(width), height_(height) {
        if (context_.initialize(window)) ready_ = true;
    }
    ~SceneSurface() override { context_.shutdown(); }
    [[nodiscard]] bool ready() const { return ready_; }

    rendering::VkContext* getVkContext() const override { return &context_; }
    int getWidth() const override { return width_; }
    int getHeight() const override { return height_; }
    int getDrawableWidth() const override { return width_; }
    int getDrawableHeight() const override { return height_; }

private:
    mutable rendering::VkContext context_;
    int width_;
    int height_;
    bool ready_ = false;
};

glm::vec3 vec3(const json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() != 3) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// The asset tree the client reads: the expansion's own when it has a
/// manifest, with the base Data/ behind it.
bool openAssets(pipeline::AssetManager& assets) {
    const char* env = std::getenv("WOW_DATA_PATH");
    const std::string dataPath = env && *env ? env : "./Data";
    const char* expansionEnv = std::getenv("WOWEE_SCENE_EXPANSION");
    const std::string expansion = expansionEnv && *expansionEnv ? expansionEnv : "wotlk";
    const std::string expansionPath = dataPath + "/expansions/" + expansion;
    std::string assetPath = dataPath;
    if (std::filesystem::exists(expansionPath + "/manifest.json")) {
        assets.setExpansionDataPath(expansionPath);
        assetPath = expansionPath;
    }
    if (!assets.initialize(assetPath)) return false;
    if (assetPath != dataPath) assets.setBaseFallbackPath(dataPath, expansion);

    // The expansion's DBC column layouts, as the client sets them: the
    // renderer reads tables through them (LiquidType.dbc among them).
    static pipeline::DBCLayout layouts;
    const std::string layoutsPath = expansionPath + "/dbc_layouts.json";
    if (layouts.loadFromJson(layoutsPath)) {
        pipeline::setActiveDBCLayout(&layouts);
    } else {
        std::fprintf(stderr, "scene_render: no DBC layouts at %s - tables are read without "
                             "them\n", layoutsPath.c_str());
    }
    return true;
}

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool valid = false;
};

/// One placed model, and when the scene brings it back into the world.
struct Placement {
    bool wmo = false;
    uint32_t instanceId = 0;
    int shownAtFrame = -1;  ///< -1: left as placed
};

/// A WMO's own doodads, placed as children of one of its instances the way
/// the client fits out a transport: each an M2 instance of its own, carried
/// by the hull's transform.
bool placeWmoDoodads(const std::string& wmoPath, uint32_t wmoModelId, uint32_t wmoInstanceId,
                     pipeline::AssetManager& assets, rendering::Renderer& renderer,
                     uint32_t& nextDoodadModelId) {
    auto* wmoRenderer = renderer.getWMORenderer();
    auto* m2Renderer = renderer.getM2Renderer();
    const auto* templates = wmoRenderer->getDoodadTemplates(wmoModelId);
    if (!templates || templates->empty()) {
        LOG_ERROR("scene_render: ", wmoPath, " has no doodads to place");
        return false;
    }
    std::unordered_map<std::string, uint32_t> modelIds;
    for (const auto& doodad : *templates) {
        auto [known, fresh] = modelIds.try_emplace(lowered(doodad.m2Path), nextDoodadModelId);
        if (fresh) {
            ++nextDoodadModelId;
            auto model = pipeline::M2Loader::load(assets.readFile(doodad.m2Path));
            model.name = doodad.m2Path;
            if (!pipeline::M2Loader::loadSkin(assets.readFile(pipeline::skinPathForM2(doodad.m2Path)),
                                              model) ||
                !m2Renderer->loadModel(model, known->second)) {
                LOG_ERROR("scene_render: cannot load ", doodad.m2Path, ", a doodad of ", wmoPath);
                return false;
            }
        }
        // At a placeholder position the hull moves it from, so never one
        // deduplicated against another doodad of the same model.
        const uint32_t instanceId = m2Renderer->createInstance(
            known->second, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f, /*allowPositionDedup=*/false);
        if (instanceId == 0) {
            LOG_ERROR("scene_render: no instance for ", doodad.m2Path, ", a doodad of ", wmoPath);
            return false;
        }
        wmoRenderer->addDoodadToInstance(wmoInstanceId, instanceId, doodad.localTransform);
    }
    LOG_INFO("scene_render: placed ", templates->size(), " doodads of ", wmoPath);
    return true;
}

/// Load one model through the client's loaders and place it. Its bounds,
/// in world space, extend `bounds`.
bool placeModel(const json& entry, const glm::vec3& origin, pipeline::AssetManager& assets,
                rendering::Renderer& renderer, uint32_t modelId, uint32_t& nextDoodadModelId,
                Bounds& bounds, std::vector<Placement>& placements) {
    const std::string path = entry.value("path", "");
    const glm::vec3 position = origin + vec3(entry.value("offset", json()));
    const glm::vec3 rotation = glm::radians(vec3(entry.value("rotation", json())));
    const float scale = entry.value("scale", 1.0f);
    const bool hidden = entry.value("hidden", false);
    Placement placement{.shownAtFrame = entry.value("shownAtFrame", -1)};
    glm::vec3 lo(0.0f), hi(0.0f);
    const std::string lower = lowered(path);
    placement.wmo = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".wmo") == 0;
    if (!placement.wmo && entry.value("doodads", false)) {
        LOG_ERROR("scene_render: \"doodads\" is for a WMO, not ", path);
        return false;
    }
    if (placement.wmo) {
        auto wmo = pipeline::WMOLoader::load(assets.readFile(path));
        for (uint32_t group = 0; group < wmo.nGroups; ++group) {
            const auto candidates = pipeline::wmoGroupCandidates(path, group);
            if (!pipeline::WMOLoader::loadGroup(assets.readFile(candidates.front()), wmo, group)) {
                LOG_ERROR("scene_render: cannot load group ", group, " of ", path);
                return false;
            }
        }
        auto* wmoRenderer = renderer.getWMORenderer();
        if (!wmoRenderer->loadModel(wmo, modelId)) return false;
        placement.instanceId = wmoRenderer->createInstance(modelId, position, rotation, scale);
        if (entry.value("doodads", false) &&
            !placeWmoDoodads(path, modelId, placement.instanceId, assets, renderer, nextDoodadModelId)) {
            return false;
        }
        if (hidden) wmoRenderer->setInstanceHidden(placement.instanceId, true);
        lo = wmo.boundingBoxMin;
        hi = wmo.boundingBoxMax;
    } else {
        auto model = pipeline::M2Loader::load(assets.readFile(path));
        model.name = path;
        if (!pipeline::M2Loader::loadSkin(assets.readFile(pipeline::skinPathForM2(path)), model)) {
            LOG_ERROR("scene_render: cannot load the skin of ", path);
            return false;
        }
        auto* m2Renderer = renderer.getM2Renderer();
        if (!m2Renderer->loadModel(model, modelId)) return false;
        placement.instanceId = m2Renderer->createInstance(modelId, position, rotation, scale, false);
        if (hidden) m2Renderer->setInstanceHidden(placement.instanceId, true);
        // From the vertices: the header's box is not always filled in, and a
        // doodad is often modelled well away from its origin.
        if (!model.vertices.empty()) {
            lo = hi = model.vertices.front().position;
            for (const auto& v : model.vertices) {
                lo = glm::min(lo, v.position);
                hi = glm::max(hi, v.position);
            }
        }
    }
    lo = position + lo * scale;
    hi = position + hi * scale;
    if (!bounds.valid) {
        bounds = {lo, hi, true};
    } else {
        bounds.min = glm::min(bounds.min, lo);
        bounds.max = glm::max(bounds.max, hi);
    }
    placements.push_back(placement);
    LOG_INFO("scene_render: placed ", path, hidden ? " hidden" : "");
    return true;
}

/// A unit's model through the client's loaders, placed as the client places
/// units, to be moved each frame at `velocity`.
struct MovingCharacter {
    uint32_t instance = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
};

bool placeCharacter(const json& entry, const glm::vec3& origin, pipeline::AssetManager& assets,
                    rendering::Renderer& renderer, uint32_t modelId, MovingCharacter& placed) {
    const std::string path = entry.value("path", "");
    auto* characters = renderer.getCharacterRenderer();
    if (!characters) {
        LOG_ERROR("scene_render: no character renderer to place ", path, " with");
        return false;
    }
    auto model = pipeline::M2Loader::load(assets.readFile(path));
    model.name = path;
    if (!pipeline::M2Loader::loadSkin(assets.readFile(pipeline::skinPathForM2(path)), model)) {
        LOG_ERROR("scene_render: cannot load the skin of ", path);
        return false;
    }
    if (!characters->loadModel(model, modelId)) {
        LOG_ERROR("scene_render: the character renderer would not take ", path);
        return false;
    }
    placed.position = origin + vec3(entry.value("offset", json()));
    placed.velocity = vec3(entry.value("velocity", json()));
    placed.instance = characters->createInstance(modelId, placed.position,
                                                 glm::radians(vec3(entry.value("rotation", json()))),
                                                 entry.value("scale", 1.0f));
    if (placed.instance == 0) {
        LOG_ERROR("scene_render: cannot place ", path);
        return false;
    }
    LOG_INFO("scene_render: placed character ", path);
    return true;
}

/// What the scene left in video memory, by use, and what each texture cache
/// counts itself as holding: the world's share of a city, without a realm.
/// WOWEE_GPU_MEMORY_DUMP names a file for the allocator's whole map.
void reportVideoMemory(const rendering::Renderer& renderer) {
    const rendering::VkContext* vk = renderer.getVkContext();
    if (!vk) return;
    constexpr double kMiB = 1024.0 * 1024.0;
    if (const auto heap = vk->deviceLocalHeap()) {
        LOG_INFO("scene_render: video memory ", heap->usageBytes / kMiB, " MB of ",
                 heap->budgetBytes / kMiB, " MB budget");
    }
    if (const auto byUse = vk->gpuMemoryByUse()) {
        for (size_t i = 0; i < rendering::kGpuMemoryUseCount; ++i) {
            const auto use = static_cast<rendering::GpuMemoryUse>(i);
            LOG_INFO("scene_render: video memory for ", rendering::gpuMemoryUseName(use), ": ",
                     byUse->deviceLocal(use) / kMiB, " MB");
        }
    }
    const auto textures = vk->textures().stats();
    LOG_INFO("scene_render: texture cache ", textures.bytes / kMiB, " MB in ", textures.entries,
             " entries, budget ", textures.budgetBytes / kMiB, " MB, ", textures.refusals, " refused");
    for (size_t i = 0; i < rendering::kTextureFormCount; ++i) {
        const auto form = static_cast<rendering::TextureForm>(i);
        LOG_INFO("scene_render: texture cache ", rendering::textureFormName(form), ": ",
                 textures.form(form).bytes / kMiB, " MB in ", textures.form(form).entries, " entries");
    }
    if (const char* path = std::getenv("WOWEE_GPU_MEMORY_DUMP"); path && *path) {
        if (vk->writeGpuMemoryMap(path)) LOG_INFO("scene_render: allocator map written to ", path);
    }
}

/// Per-frame timing (--timing): a line for each frame, and the frame budget
/// of those after the warm-up, whose first frames build pipelines.
class FrameTimer {
public:
    FrameTimer(bool enabled, int warmupFrames) : enabled_(enabled), warmup_(warmupFrames) {}

    /// Time one renderer call of the frame.
    template <typename Fn>
    void call(const char* name, Fn&& fn) {
        if (!enabled_) {
            fn();
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        fn();
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        calls_.emplace_back(name, ms);
    }

    void frameDone(int frame, float frameMs, rendering::VkContext& vk) {
        if (!enabled_) return;
        const bool counted = frame >= warmup_;
        std::string line = "frame " + std::to_string(frame) + ": " + fixed(frameMs) + " ms";
        for (const auto& [name, ms] : calls_) {
            line += std::string(", ") + name + " " + fixed(ms);
            if (counted) budget_.noteStage(name, ms);
        }
        // The renderer's own parts, largest three on the line, all of them
        // in the budget.
        auto parts = std::vector<rendering::CpuStageTimes::Entry>(vk.cpuStages().entries().begin(),
                                                                 vk.cpuStages().entries().end());
        for (const auto& part : parts) {
            if (counted) budget_.noteStage(std::string(part.group) + "/" + std::string(part.name), part.milliseconds);
        }
        std::sort(parts.begin(), parts.end(),
                  [](const auto& a, const auto& b) { return a.milliseconds > b.milliseconds; });
        line += " [";
        for (size_t i = 0; i < parts.size() && i < 3; ++i) {
            line += (i ? ", " : "") + std::string(parts[i].group) + "/" + std::string(parts[i].name) + " " +
                    fixed(parts[i].milliseconds);
        }
        line += "]";
        vk.cpuStages().clear();
        if (vk.gpuTimingsSerial() != gpuSeen_) {
            gpuSeen_ = vk.gpuTimingsSerial();
            double gpu = 0.0;
            for (const auto& [label, ms] : vk.gpuTimings()) gpu += ms;
            line += ", GPU " + fixed(static_cast<float>(gpu)) + " ms (a frame in flight earlier)";
            if (counted) budget_.noteGpuFrame(vk.gpuTimings());
        }
        if (counted) {
            budget_.frameFinished(frameMs);
            windowMs_ += frameMs;
        }
        std::printf("timing: %s\n", line.c_str());
        calls_.clear();
    }

    void report(const rendering::VkContext& vk) const {
        if (!enabled_) return;
        for (const auto& line : budget_.report("scene_render, frames after the first " +
                                                   std::to_string(warmup_), windowMs_)) {
            std::printf("timing: %s\n", line.c_str());
        }
        if (*vk.gpuTimingCaveat() != '\0') std::printf("timing:     %s\n", vk.gpuTimingCaveat());
    }

private:
    static std::string fixed(float ms) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", ms);
        return text;
    }

    bool enabled_;
    int warmup_;
    std::vector<std::pair<const char*, float>> calls_;
    core::FrameBudget budget_;
    uint64_t gpuSeen_ = 0;
    double windowMs_ = 0.0;
};

int render(const json& scene, const std::string& outPath, pipeline::AssetManager& assets,
           rendering::Renderer& renderer, bool timing) {
    const std::string map = scene.value("map", "Northrend");
    if (!renderer.initializeRenderers(&assets, map)) return 1;
    // The minimap is interface; a scene shows only the world unless asked.
    if (auto* minimap = renderer.getMinimap()) minimap->setEnabled(scene.value("minimap", false));

    glm::vec3 origin(5818.0f, 405.0f, 650.0f);
    if (scene.contains("server")) {
        origin = core::coords::canonicalToRender(core::coords::serverToCanonical(vec3(scene["server"])));
    }
    if (scene.contains("origin")) origin = vec3(scene["origin"]);

    if (scene.value("terrain", false)) {
        const glm::vec3 canonical = core::coords::renderToCanonical(origin);
        const auto [tileX, tileY] = core::coords::canonicalToTile(canonical.x, canonical.y);
        const std::string adt = "World\\Maps\\" + map + "\\" + map + "_" + std::to_string(tileX) +
                                "_" + std::to_string(tileY) + ".adt";
        if (!renderer.loadTestTerrain(&assets, adt)) {
            std::fprintf(stderr, "scene_render: no terrain at %s\n", adt.c_str());
            return 1;
        }
        // The tiles around it, not the client's view distance of them: the
        // frame waits for every tile it asks for.
        const int radius = scene.value("terrainRadius", 1);
        renderer.getTerrainManager()->setLoadRadius(radius);
        renderer.getTerrainManager()->setUnloadRadius(radius + 2);
    }

    Bounds bounds;
    uint32_t modelId = 0xFFFF0400;
    uint32_t doodadModelId = 0xFFFE0000;
    std::vector<Placement> placements;
    for (const auto& entry : scene.value("models", json::array())) {
        if (!placeModel(entry, origin, assets, renderer, modelId++, doodadModelId, bounds, placements)) {
            return 1;
        }
    }
    std::vector<MovingCharacter> movingCharacters;
    for (const auto& entry : scene.value("characters", json::array())) {
        MovingCharacter placed;
        if (!placeCharacter(entry, origin, assets, renderer, modelId++, placed)) return 1;
        movingCharacters.push_back(placed);
    }

    // The camera: placed, aimed, or backed off to see the first model.
    auto* camera = renderer.getCamera();
    camera->setFov(scene.value("fov", 60.0f));
    const glm::vec3 centre = bounds.valid ? (bounds.min + bounds.max) * 0.5f : origin;
    const json cam = scene.value("camera", json::object());
    glm::vec3 eye;
    if (cam.contains("position")) {
        eye = vec3(cam["position"]);
    } else if (cam.contains("offset")) {
        eye = origin + vec3(cam["offset"]);
    } else {
        const float reach = bounds.valid ? std::max(glm::length(bounds.max - bounds.min), 1.0f) : 10.0f;
        eye = centre - glm::vec3(reach, 0.0f, 0.0f);
    }
    float yaw = cam.value("yaw", 0.0f);
    float pitch = cam.value("pitch", 0.0f);
    if (cam.contains("lookAt") || !(cam.contains("yaw") || cam.contains("pitch"))) {
        const glm::vec3 target = cam.contains("lookAt") ? origin + vec3(cam["lookAt"]) : centre;
        const glm::vec3 d = target - eye;
        yaw = glm::degrees(std::atan2(d.y, d.x));
        pitch = glm::degrees(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)));
    }
    camera->setPosition(eye);
    camera->setRotation(yaw, pitch);
    // Light, shadows and fog centre on the character; here that is the scene.
    renderer.getCharacterPosition() = centre;

    rendering::WorldEnvironment environment;
    environment.mapId = scene.value("mapId", 571u);
    environment.zoneId = scene.value("zoneId", 0u);
    environment.gameTime = scene.value("hour", 12.0f);
    const std::string weatherName = scene.value("weather", std::string("clear"));
    const auto weatherKind = pipeline::weatherKindFromName(weatherName == "none" ? "clear" : weatherName);
    if (!weatherKind) {
        std::fprintf(stderr, "scene_render: \"weather\" is one of clear, fog, rain, snow, sandstorm, "
                             "thunderstorm, blackrain, blacksnow - not \"%s\"\n", weatherName.c_str());
        return 1;
    }
    environment.weatherKind = *weatherKind;
    environment.weatherIntensity = scene.value("weatherIntensity", 0.0f);
    // What of it is drawn, as the weather dev panel switches it.
    renderer.weatherControls().lens = scene.value("lens", true);
    renderer.weatherControls().splashes = scene.value("splashes", true);
    // A realm clock of the scene's own, so the wind - drawn from it - is the
    // same on every run rather than whatever this machine's clock says.
    const double realmTime = scene.value("realmTime", 0.0);
    environment.realmSeconds = realmTime;
    if (scene.contains("wind")) {
        const json& wind = scene["wind"];
        if (!wind.is_object() || !wind.contains("direction") || !wind["direction"].is_number() ||
            !wind.contains("speed") || !wind["speed"].is_number() || wind["speed"].get<float>() < 0.0f) {
            std::fprintf(stderr, "scene_render: \"wind\" wants { \"direction\": degrees, "
                                 "\"speed\": yards a second, zero or more }\n");
            return 1;
        }
        environment.wind = rendering::WindOverride{wind["direction"].get<float>(),
                                                   wind["speed"].get<float>()};
    }
    renderer.setWorldEnvironment(environment);
    renderer.setWorldInView(true);

    // The atmosphere settings, as the settings panel sets them. Unset, each
    // keeps the renderer's own default, not the panel's.
    if (auto* lighting = renderer.getLightingManager()) {
        if (scene.contains("fogStrength")) lighting->setFogStrength(scene["fogStrength"].get<float>());
        if (scene.contains("fogSkyBlend")) lighting->setFogSkyBlend(scene["fogSkyBlend"].get<float>());
    }
    if (scene.contains("volumetricFog")) renderer.setVolumetricFogQuality(scene["volumetricFog"].get<int>());
    if (scene.contains("volumetricDensity")) {
        renderer.setVolumetricFogDensity(scene["volumetricDensity"].get<float>());
    }
    if (scene.contains("sunShafts")) renderer.setSunShaftsEnabled(scene["sunShafts"].get<bool>());

    // Anti-aliasing as the settings panel sets it: the sample count, and
    // FXAA, which puts the scene in an off-screen target of its own.
    const int msaa = scene.value("msaa", 1);
    if (msaa != 1 && msaa != 2 && msaa != 4 && msaa != 8) {
        std::fprintf(stderr, "scene_render: \"msaa\" is 1, 2, 4 or 8 samples\n");
        return 1;
    }
    if (msaa > 1) renderer.setMsaaSamples(static_cast<VkSampleCountFlagBits>(msaa));
    if (scene.value("fxaa", false)) {
        if (auto* post = renderer.getPostProcessPipeline()) {
            post->setFXAAEnabled(true);
        } else {
            std::fprintf(stderr, "scene_render: \"fxaa\" asked for but there is no post-processing\n");
            return 1;
        }
    }

    // Fixed steps, so the same scene draws the same frame. Terrain streams in
    // on its own threads: with it on, the frames wait for it as well.
    const int frames = std::max(1, scene.value("frames", 30));
    constexpr float kStep = 1.0f / 30.0f;
    const auto* terrain = renderer.getTerrainManager();
    const bool waitTerrain = scene.value("terrain", false) && terrain;
    int frame = 0;
    FrameTimer timer(timing, std::min(10, frames / 3));
    auto drawFrame = [&]() {
        const auto start = std::chrono::steady_clock::now();
        for (const auto& placement : placements) {
            if (placement.shownAtFrame != frame) continue;
            if (placement.wmo) {
                renderer.getWMORenderer()->setInstanceHidden(placement.instanceId, false);
            } else {
                renderer.getM2Renderer()->setInstanceHidden(placement.instanceId, false);
            }
        }
        environment.realmSeconds = realmTime + frame * static_cast<double>(kStep);
        renderer.setWorldEnvironment(environment);
        for (MovingCharacter& c : movingCharacters) {
            if (frame > 0) c.position += c.velocity * kStep;
            renderer.getCharacterRenderer()->setInstancePosition(c.instance, c.position);
        }
        timer.call("rendererUpdate", [&] { renderer.update(kStep); });
        timer.call("beginFrame", [&] { renderer.beginFrame(); });
        timer.call("renderWorld", [&] { renderer.renderWorld(); });
        timer.call("endFrame", [&] { renderer.endFrame(); });
        timer.frameDone(frame, std::chrono::duration<float, std::milli>(
                                   std::chrono::steady_clock::now() - start).count(),
                        *renderer.getVkContext());
        ++frame;
    };
    for (int extra = 0; frame < frames || (waitTerrain && extra < 600 &&
                                           terrain->getRemainingTileCount() > 0);) {
        if (frame >= frames) ++extra;
        drawFrame();
    }

    // Then, if asked, the camera on the move for the last few frames. What a
    // pass keeps of an earlier frame - the water's refraction once kept the
    // whole finished picture - only shows while the view is changing.
    if (cam.contains("velocity")) {
        const glm::vec3 velocity = vec3(cam["velocity"]);
        const int motionFrames = std::max(1, cam.value("motionFrames", 10));
        for (int i = 1; i <= motionFrames; ++i) {
            camera->setPosition(eye + velocity * (kStep * static_cast<float>(i)));
            drawFrame();
        }
    }
    timer.report(*renderer.getVkContext());
    if (renderer.weatherMix().any()) {
        const auto& mix = renderer.weatherMix();
        LOG_WARNING("scene_render: weather rain ", mix.rain, " snow ", mix.snow, " sand ", mix.sand,
                    "; ", renderer.precipitationCount(), " drops drawn; camera open to the sky ",
                    renderer.skyExposure(), ", sky in view ", renderer.skyView(), "; ",
                    renderer.lensDrops().drops().size(), " on the lens, frost ",
                    renderer.lensDrops().frost(), ", dust ", renderer.lensDrops().dust());
    }
    std::printf("scene_render: world M2 draw calls: %u in the kept frame\n",
                renderer.getM2Renderer()->getDrawCallCount());
    if (!renderer.captureScreenshot(outPath)) {
        std::fprintf(stderr, "scene_render: could not write %s\n", outPath.c_str());
        return 1;
    }
    reportVideoMemory(renderer);
    return 0;
}

int run(const json& scene, const std::string& outPath, bool timing) {
    const auto start = std::chrono::steady_clock::now();
    const json size = scene.value("size", json::array({1280, 720}));
    const int width = size[0].get<int>();
    const int height = size[1].get<int>();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("scene_render", width, height,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int status = 1;
    {
        SceneSurface surface(window, width, height);
        pipeline::AssetManager assets;
        rendering::Renderer renderer;
        if (!surface.ready()) {
            std::fprintf(stderr, "scene_render: no Vulkan device\n");
        } else if (!openAssets(assets)) {
            std::fprintf(stderr, "scene_render: no assets under WOW_DATA_PATH or ./Data\n");
        } else {
            renderer.setShadowQualityLevel(scene.value("shadowQuality", 2));
            if (renderer.initialize(&surface) &&
                renderer.getLightingManager()->initialize(&assets)) {
                status = render(scene, outPath, assets, renderer, timing);
            } else {
                std::fprintf(stderr, "scene_render: the renderer did not start\n");
            }
            renderer.shutdown();
        }
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (status == 0) std::printf("%s (%.1f s)\n", outPath.c_str(), seconds);
    return status;
}

} // namespace

int main(int argc, char** argv) {
    const bool timing = argc == 4 && std::string(argv[3]) == "--timing";
    if (argc != 3 && !timing) {
        std::fprintf(stderr, "usage: scene_render <scene.json> <out.png> [--timing]\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "scene_render: cannot read %s\n", argv[1]);
        return 2;
    }
    json scene;
    try {
        scene = json::parse(in, nullptr, true, /*ignore_comments=*/true);
    } catch (const json::exception& e) {
        std::fprintf(stderr, "scene_render: %s: %s\n", argv[1], e.what());
        return 2;
    }
    return run(scene, argv[2], timing);
}
