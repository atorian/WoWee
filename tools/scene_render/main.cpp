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
//     "lightning": { "start": [70, 70, 160], // a fixed strike after streaming,
//                    "end": [70, 70, 0],    // offsets from origin, render axes;
//                    "age": 0.5, "seed": 17 }, // seconds after its leader starts
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
//     "playerPosition": [5815,629,647],  // lighting/weather sample position,
//                                         // render axes; default: model centre
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
//     "rayTracedLighting": 0,             // RT lighting mode 0-3
//     "msaa": 1,                          // samples: 1, 2, 4 or 8
//     "bloom": 0.8,                       // bright light glow, 0 disables
//     "fxaa": false,                      // FXAA, drawn through its own target
//     "clouds": true,                   // procedural layer; authored sky models keep their gate
//     "sharpStars": true,                 // point stars in place of baked star layers
//     "viewDistance": 1900.0,             // world geometry range, in yards
//     "fogStrength": 0.4,                 // the settings panel's atmosphere:
//     "rayTracedLighting": 3,             // RT mode: 0 off, 1 shadows, 2 +AO, 3 +GI
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
#include "core/frame_worker.hpp"
#include "core/frame_budget.hpp"
#include "rendering/minimap.hpp"
#include "rendering/lightning.hpp"
#include "rendering/frame_sink.hpp"
#include "rendering/frame_scene.hpp"
#include "stb_image_write.h"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/clouds.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/frame_sink.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/render_surface.hpp"
#include "rendering/renderer.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/terrain_renderer.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/wmo_renderer.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include "stb_image_write.h"
#include "interface_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
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

/// Read the kept frame before presentation, using the renderer's fenced
/// readback. Reading a swapchain image after present has released it is invalid.
class SceneCapture final : public rendering::FrameSink {
public:
    SceneCapture(rendering::Renderer& renderer, const int& frame, int firstFrame)
        : renderer_(renderer), frame_(frame), firstFrame_(firstFrame) {
        const auto extent = renderer.getVkContext()->getSwapchainExtent();
        width_ = extent.width;
        height_ = extent.height;
        rgba_.resize(static_cast<size_t>(width_) * height_ * 4);
        if (!renderer_.beginFrameCapture(this, width_, height_)) {
            LOG_ERROR("scene_render: could not start the kept frame's GPU readback");
            throw std::runtime_error("Scene frame readback failed");
        }
    }
    ~SceneCapture() override { renderer_.endFrameCapture(); }

    bool frameDue(int64_t* pts) const override {
        *pts = frame_;
        return frame_ >= firstFrame_ && frame_ > taken_;
    }
    void frameTaken(int64_t pts) override { taken_ = pts; }
    void frameDropped() override {
        LOG_ERROR("scene_render: kept frame readback dropped; no free GPU readback buffer");
        dropped_ = true;
    }
    void submit(Frame input) override {
        if (input.pts >= kept_) {
            for (uint32_t y = 0; y < height_; ++y) {
                const auto* src = input.bgra + static_cast<size_t>(y) * input.stride;
                auto* dst = rgba_.data() + static_cast<size_t>(y) * width_ * 4;
                for (uint32_t x = 0; x < width_; ++x) {
                    dst[x * 4] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                }
            }
            kept_ = input.pts;
        }
        input.release();
    }
    double elapsedSeconds() const override { return 0.0; }

    bool write(const std::string& path) {
        renderer_.flushFrameCapture();
        if (dropped_ || kept_ != frame_ - 1) {
            LOG_ERROR("scene_render: final frame ", frame_ - 1,
                      " was not read back; latest frame is ", kept_);
            return false;
        }
        return stbi_write_png(path.c_str(), width_, height_, 4, rgba_.data(), width_ * 4) != 0;
    }

private:
    rendering::Renderer& renderer_;
    const int& frame_;
    int firstFrame_;
    int64_t taken_ = -1, kept_ = -1;
    uint32_t width_ = 0, height_ = 0;
    bool dropped_ = false;
    std::vector<uint8_t> rgba_;
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
    uint32_t modelId = 0;
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
    Placement placement{.modelId = modelId, .shownAtFrame = entry.value("shownAtFrame", -1)};
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
    // Isolated renderer fixtures can keep background streaming out of their
    // clock: asset readiness must not change a frame-input isolation test.
    if (!scene.value("streamWorld", true) && renderer.getTerrainManager())
        renderer.getTerrainManager()->setStreamingEnabled(false);
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
    // Authored-order material layers through the ordinary WMO upload/scene API.
    // Kept in scene_render only: no fixture switch in the shipping renderer.
    // Deterministic material/instancing fixture through the production M2 API.
    for(const char* family : {"m2Layers","characterLayers"}) if(scene.contains(family)) {
        const bool character=std::string_view(family)=="characterLayers";
        const auto& layers=scene.at(family);
        if(!layers.is_array() || layers.empty() || layers.size()>64)
            throw std::runtime_error("m2Layers requires 1..64 rectangular layers");
        pipeline::M2Model model{};model.name="scene_render_materials.m2";
        model.bones.resize(1);model.bones[0].parentBone=-1;
        model.boundMin=glm::vec3(1e10f);model.boundMax=glm::vec3(-1e10f);
        std::unordered_map<std::string,pipeline::BLPImage> pixels;
        for(const auto& layer:layers) {
            const auto color=layer.at("rgba").get<std::array<uint8_t,4>>();
            const auto rect=layer.at("rect").get<std::array<float,4>>();
            const float x=layer.at("x").get<float>();
            pipeline::M2Batch batch{};
            batch.colorIndex=batch.transparencyIndex=batch.textureAnimIndex=UINT16_MAX;
            batch.textureIndex=uint16_t(model.textureLookup.size());
            batch.materialIndex=uint16_t(model.materials.size());
            batch.textureCount=layer.contains("secondRgba")?2:1;
            batch.shader=layer.value("shader",uint16_t(0));
            model.materials.push_back({layer.value("flags",uint16_t(0)),layer.value("blend",uint16_t(0))});
            for(uint16_t stage=0;stage<batch.textureCount;++stage) {
                const auto rgba=stage?layer.at("secondRgba").get<std::array<uint8_t,4>>():color;
                const auto id=uint16_t(model.textures.size());
                const std::string texture=std::string("scene_render_")+(character?"character":"m2")+"_material_"+std::to_string(id)+".blp";
                pipeline::BLPImage image;image.width=image.height=16;image.data.resize(16*16*4);
                for(int y=0;y<16;++y)for(int z=0;z<16;++z) {
                    std::copy(rgba.begin(),rgba.end(),image.data.begin()+(y*16+z)*4);
                    if(stage==0 && layer.value("cutout",false) && ((y/4+z/4)%2))image.data[(y*16+z)*4+3]=0;
                }
                pixels.emplace(texture,std::move(image));model.textures.push_back({0,0,texture});model.textureLookup.push_back(id);
            }
            batch.vertexStart=uint32_t(model.vertices.size());batch.vertexCount=4;
            batch.indexStart=uint32_t(model.indices.size());batch.indexCount=6;
            const std::array<glm::vec2,4> corners{{{rect[0],rect[1]},{rect[2],rect[1]},
                                                {rect[2],rect[3]},{rect[0],rect[3]}}};
            for(size_t i=0;i<4;++i) {
                pipeline::M2Vertex vertex{};vertex.position={x,corners[i].x,corners[i].y};
                vertex.normal={-1,0,0};vertex.boneWeights[0]=255;
                vertex.texCoords[0]={i==1||i==2?1.f:0.f,i>=2?1.f:0.f};vertex.texCoords[1]=vertex.texCoords[0];
                model.vertices.push_back(vertex);model.boundMin=glm::min(model.boundMin,vertex.position);
                model.boundMax=glm::max(model.boundMax,vertex.position);
            }
            for(uint16_t i:{0,1,2,0,2,3})model.indices.push_back(uint16_t(batch.vertexStart)+i);
            model.batches.push_back(batch);
        }
        model.boundRadius=glm::length(model.boundMax-model.boundMin)*0.5f;
        auto& m2=*renderer.getM2Renderer();
        auto& characters=*renderer.getCharacterRenderer();
        if(character)characters.setPredecodedBLPCache(&pixels);else m2.setPredecodedBLPCache(&pixels);
        const bool loaded=character?characters.loadModel(model,modelId):m2.loadModel(model,modelId);
        if(character)characters.setPredecodedBLPCache(nullptr);else m2.setPredecodedBLPCache(nullptr);
        if(!loaded)throw std::runtime_error("scene_render material layers upload failed");
        const auto instances=scene.value(character?"characterLayerInstances":"m2LayerInstances",json::array({json::object()}));
        for(const auto& instance:instances) {
            const glm::vec3 offset=vec3(instance.value("offset",json()));
            const uint32_t id=character?characters.createInstance(modelId,origin+offset,glm::vec3(0),1):
                m2.createInstance(modelId,origin+offset,glm::vec3(0),1,false);
            if(!id)throw std::runtime_error("scene_render layer instance creation failed");
            if(character) {
                characters.setInstanceOpacity(id,instance.value("opacity",1.0f));
                movingCharacters.push_back({.instance=id,.position=origin+offset,.velocity=glm::vec3(0)});
            } else {
                if(instance.value("hidden",false))m2.setInstanceHidden(id,true);
                if(instance.contains("interiorAmbient"))m2.setInteriorLight(id,rendering::InteriorLight{
                    vec3(instance.at("interiorAmbient")),vec3(instance.at("interiorDirect"))});
                placements.push_back({.instanceId=id,.modelId=modelId});
            }
            const auto lo=origin+offset+model.boundMin,hi=origin+offset+model.boundMax;
            if(!bounds.valid)bounds={lo,hi,true};else {bounds.min=glm::min(bounds.min,lo);bounds.max=glm::max(bounds.max,hi);}
        }
        ++modelId;
        LOG_INFO("scene_render: placed ",layers.size()," material layers for ",family," in ",instances.size()," instances");
    }
    if (scene.contains("wmoLayers")) {
        const auto& layers=scene["wmoLayers"];
        if (!layers.is_array() || layers.empty() || layers.size()>64)
            throw std::runtime_error("wmoLayers requires 1..64 rectangular layers");
        pipeline::WMOModel model{};
        model.sourcePath="scene_render_layers.wmo";
        model.nGroups=1; model.flags=pipeline::kWMOModelUnifiedRenderPath;
        model.ambientColor=glm::vec3(0.2f);
        pipeline::WMOGroup group{};
        group.flags=8|pipeline::kWMOGroupHasVertexColors;
        group.boundingBoxMin=glm::vec3(1e10f);group.boundingBoxMax=glm::vec3(-1e10f);
        std::unordered_map<std::string,pipeline::BLPImage> pixels;
        for (const auto& layer:layers) {
            const auto id=static_cast<uint8_t>(model.materials.size());
            const auto color=layer.at("rgba").get<std::array<uint8_t,4>>();
            const auto rect=layer.at("rect").get<std::array<float,4>>();
            const float x=layer.at("x").get<float>();
            const std::string texture="scene_render_layer_"+std::to_string(id)+".blp";
            pipeline::BLPImage image;image.width=image.height=16;image.data.resize(16*16*4);
            for (int y=0;y<16;++y) for (int z=0;z<16;++z) {
                std::copy(color.begin(),color.end(),image.data.begin()+(y*16+z)*4);
                if (layer.value("cutout",false) && ((y/4+z/4)%2)) image.data[(y*16+z)*4+3]=0;
            }
            pixels.emplace(texture,std::move(image));
            model.textureOffsetToIndex[id]=id;model.textures.push_back(texture);
            pipeline::WMOMaterial material{};material.texture1=id;
            material.blendMode=layer.value("blend",0u);material.flags=layer.value("flags",1u);
            model.materials.push_back(material);
            const uint16_t first=static_cast<uint16_t>(group.vertices.size());
            const std::array<glm::vec2,4> corners{{{rect[0],rect[1]},{rect[2],rect[1]},
                                                {rect[2],rect[3]},{rect[0],rect[3]}}};
            for (size_t i=0;i<4;++i) {
                pipeline::WMOVertex vertex{};vertex.position={x,corners[i].x,corners[i].y};
                vertex.normal={-1,0,0};vertex.color={1,1,1,1};
                vertex.texCoord={i==1||i==2?1.f:0.f,i>=2?1.f:0.f};
                group.vertices.push_back(vertex);
                group.boundingBoxMin=glm::min(group.boundingBoxMin,vertex.position);
                group.boundingBoxMax=glm::max(group.boundingBoxMax,vertex.position);
            }
            pipeline::WMOBatch batch{};batch.startIndex=static_cast<uint32_t>(group.indices.size());
            batch.indexCount=6;batch.startVertex=first;batch.lastVertex=first+3;batch.materialId=id;
            group.batches.push_back(batch);
            for (uint16_t i: {0,1,2,0,2,3}) group.indices.push_back(first+i);
        }
        model.nTextures=static_cast<uint32_t>(model.textures.size());
        model.boundingBoxMin=group.boundingBoxMin;model.boundingBoxMax=group.boundingBoxMax;
        model.groupInfo.push_back({group.flags,group.boundingBoxMin,group.boundingBoxMax,0});
        model.groups.push_back(std::move(group));
        auto& wmo=*renderer.getWMORenderer();
        wmo.setPredecodedBLPCache(&pixels);
        const bool loaded=wmo.loadModel(model,modelId);
        wmo.setPredecodedBLPCache(nullptr);
        if (!loaded) throw std::runtime_error("scene_render WMO layers upload failed");
        const uint32_t instance=wmo.createInstance(modelId,origin,glm::vec3(0),1);
        placements.push_back({.wmo=true,.instanceId=instance,.modelId=modelId++});
        const auto lo=origin+model.boundingBoxMin,hi=origin+model.boundingBoxMax;
        if (!bounds.valid) bounds={lo,hi,true};
        else {bounds.min=glm::min(bounds.min,lo);bounds.max=glm::max(bounds.max,hi);}
        LOG_INFO("scene_render: placed ",layers.size()," ordered WMO layers");
    }
    for (const auto& entry : scene.value("characters", json::array())) {
        MovingCharacter placed;
        if (!placeCharacter(entry, origin, assets, renderer, modelId++, placed)) return 1;
        movingCharacters.push_back(placed);
    }

    // Optional ground/liquid primitives for isolated scenes without streaming.
    if (scene.contains("groundPlane")) {
        auto mesh = std::make_unique<pipeline::TerrainMesh>();
        mesh->validChunkCount = 1;
        auto& chunk = mesh->chunks[0];
        const float size = scene["groundPlane"].value("size", 60.0f);
        const float height = origin.z + scene["groundPlane"].value("height", -2.0f);
        chunk.worldX = origin.x; chunk.worldY = origin.y; chunk.worldZ = height;
        chunk.chunkX = chunk.chunkY = 0;
        for (const auto& corner : std::array<glm::vec2, 4>{{{-1,-1}, {1,-1}, {1,1}, {-1,1}}}) {
            pipeline::TerrainVertex vertex;
            vertex.position[0] = origin.x + corner.x * size * 0.5f;
            vertex.position[1] = origin.y + corner.y * size * 0.5f;
            vertex.position[2] = height;
            vertex.normal[2] = 1.0f;
            vertex.texCoord[0] = vertex.layerUV[0] = (corner.x + 1) * 0.5f;
            vertex.texCoord[1] = vertex.layerUV[1] = (corner.y + 1) * 0.5f;
            chunk.vertices.push_back(vertex);
        }
        chunk.indices = {0,1,2, 0,2,3};
        chunk.normalGrid.resize(pipeline::ChunkMesh::kNormalGridSize * pipeline::ChunkMesh::kNormalGridSize * 4);
        for (size_t i = 0; i < chunk.normalGrid.size(); i += 4) {
            chunk.normalGrid[i] = chunk.normalGrid[i + 1] = 128;
            chunk.normalGrid[i + 2] = chunk.normalGrid[i + 3] = 255;
        }
        if (!renderer.getTerrainRenderer()->loadTerrain(*mesh, {}, 0, 0)) return 1;
    }
    if (scene.value("waterPlane", false)) {
        pipeline::WMOLiquid liquid;
        const int tiles = std::clamp(scene.value("waterTiles", 8), 1, 64);
        liquid.xTiles = liquid.yTiles = tiles;
        liquid.xVerts = liquid.yVerts = tiles + 1;
        const float halfWidth = tiles * (533.33333f / 128.0f) * 0.5f;
        liquid.basePosition = origin + glm::vec3(-halfWidth, -halfWidth, -1.0f);
        liquid.liquidType = 1;
        liquid.heights.assign((tiles + 1) * (tiles + 1), liquid.basePosition.z);
        liquid.flags.assign(tiles * tiles, 0);
        if (!renderer.liquidScene().addWmoLiquid(liquid, glm::mat4(1.0f), 1, rendering::LiquidShelter::Open)) return 1;
    }
    if (scene.value("selection", false)) renderer.setSelectionCircle(origin, 2.0f, {0, 1, 0});

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
    // A distant model's centre can lie in a different lighting volume from
    // the observer. Fixtures may fix the player's sampling position while
    // preserving model-centred lighting for existing isolated model scenes.
    renderer.getCharacterPosition() = scene.contains("playerPosition")
        ? vec3(scene["playerPosition"]) : centre;

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
    if (scene.contains("viewDistance")) {
        renderer.setViewDistance(scene["viewDistance"].get<float>());
        std::printf("scene_render: view distance %g, terrain load radius %d\n",
                    renderer.getViewDistance(), renderer.getTerrainLoadRadius());
    }

    // The atmosphere settings, as the settings panel sets them. Unset, each
    // keeps the renderer's own default, not the panel's.
    if (auto* lighting = renderer.getLightingManager()) {
        if (scene.contains("fogStrength")) lighting->setFogStrength(scene["fogStrength"].get<float>());
        if (scene.contains("fogSkyBlend"))
            LOG_WARNING("scene_render: fogSkyBlend is obsolete; the zone's authored fog colour is used");
    }
    if (scene.contains("rayTracedLighting")) {
        renderer.setRtLightingMode(scene["rayTracedLighting"].get<int>());
    }
    renderer.setRtDistance(scene.value("rtDistance", rendering::kDefaultRtDistance));
    renderer.setSharpStars(scene.value("sharpStars", true));
    if (auto* sky = renderer.getSkySystem(); sky && sky->getClouds())
        sky->getClouds()->setEnabled(scene.value("clouds", true));
    if (scene.contains("volumetricFog")) renderer.setVolumetricFogQuality(scene["volumetricFog"].get<int>());
    if (scene.contains("volumetricDensity")) {
        renderer.setVolumetricFogDensity(scene["volumetricDensity"].get<float>());
    }
    if (auto* post = renderer.getPostProcessPipeline()) post->setBloomStrength(scene.value("bloom", 0.8f));
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
    if (scene.contains("fsr2")) renderer.setFSR2Enabled(scene["fsr2"].get<bool>());
    if (scene.contains("fsrScale") && renderer.getPostProcessPipeline())
        renderer.getPostProcessPipeline()->setFSRQuality(scene["fsrScale"].get<float>());

    renderer.setRtLightingMode(scene.value("rayTracedLighting", 0));

    // Fixed steps, so the same scene draws the same frame. Terrain streams in
    // on its own threads, as long as it takes; with it on, it streams first,
    // with the scene's clock held, and the frames run from the clock's start
    // once it is in. They used to run while it streamed, and then on until it
    // was in - so how many steps the wind, the animations and the weather had
    // taken by the kept frame depended on how fast the disk was, and the same
    // scene twice drew two different frames.
    const int frames = std::max(1, scene.value("frames", 30));
    constexpr float kStep = 1.0f / 30.0f;
    const auto* terrain = renderer.getTerrainManager();
    const bool waitTerrain = scene.value("terrain", false) && terrain;
    if (waitTerrain) {
        environment.realmSeconds = realmTime;
        renderer.setWorldEnvironment(environment);
        // In when nothing is waiting and nothing more has arrived for a second
        // of frames: the manager asks for the tiles round the scene over its
        // first frames, so an empty queue on the first one is not yet the end.
        constexpr int kSettledFrames = 30;
        int waited = 0;
        int settled = 0;
        int lastLoaded = -1;
        for (; waited < scene.value("settleFrames",600) && settled < kSettledFrames; ++waited) {
            renderer.update(0.0f);
            renderer.beginFrame(renderer.extractFrameInputs());
            renderer.renderWorld();
            renderer.endFrame();
            const int loaded = terrain->getLoadedTileCount();
            settled = terrain->getRemainingTileCount() == 0 && (!scene.value("distantTerrain",false) || renderer.extractFrameInputs().distantTerrainReady) && loaded == lastLoaded ? settled + 1 : 0;
            lastLoaded = loaded;
        }
        if (terrain->getRemainingTileCount() > 0) {
            std::fprintf(stderr, "scene_render: %d terrain tiles still streaming after %d frames; "
                                 "drawing without them\n",
                         static_cast<int>(terrain->getRemainingTileCount()), waited);
        }
    }
    int frame = 0;
    SceneCapture capture(renderer, frame, frames - 1);
    std::unique_ptr<InterfaceFixture> interfaceFixture;
    if (scene.value("interface", false)) {
        const auto extent = renderer.getVkContext()->getSwapchainExtent();
        interfaceFixture = std::make_unique<InterfaceFixture>(*renderer.getVkContext(), extent.width, extent.height);
    }
    FrameTimer timer(timing, std::min(10, frames / 3));
    const char* workerMode = std::getenv("WOWEE_RENDER_THREAD");
    core::FrameWorker recordingWorker(workerMode && std::string_view(workerMode) != "0");
    auto changeWmoMaterials = [&](bool afterExtraction) {
        for (const auto& change : scene.value("wmoMaterialSettings", json::array())) {
            if (change.value("frame", 0) != frame ||
                change.value("afterExtract", false) != afterExtraction) continue;
            auto* wmo = renderer.getWMORenderer();
            if (change.contains("normalMapping")) wmo->setNormalMappingEnabled(change["normalMapping"].get<bool>());
            if (change.contains("normalStrength")) wmo->setNormalMapStrength(change["normalStrength"].get<float>());
            if (change.contains("pom")) wmo->setPOMEnabled(change["pom"].get<bool>());
            if (change.contains("pomQuality")) wmo->setPOMQuality(change["pomQuality"].get<int>());
        }
    };
    auto drawFrame = [&]() {
        const auto start = std::chrono::steady_clock::now();
        (void)renderer.takeFrameResults();
        for (const auto& change : scene.value("renderTargetChanges", json::array())) {
            if (change.at("frame").get<int>() != frame) continue;
            if (change.contains("msaa")) {
                const int samples=change["msaa"].get<int>();
                if (samples!=1 && samples!=2 && samples!=4 && samples!=8)
                    throw std::runtime_error("renderTargetChanges msaa must be 1, 2, 4 or 8");
                renderer.setMsaaSamples(static_cast<VkSampleCountFlagBits>(samples));
            }
            if (change.contains("fxaa"))
                renderer.getPostProcessPipeline()->setFXAAEnabled(change["fxaa"].get<bool>());
        }
        for (const auto& placement : placements) {
            if (placement.shownAtFrame != frame) continue;
            if (placement.wmo) {
                renderer.getWMORenderer()->setInstanceHidden(placement.instanceId, false);
            } else {
                renderer.getM2Renderer()->setInstanceHidden(placement.instanceId, false);
            }
        }
        // Scene commands exercise material row reuse without exposing renderer internals.
        for(const auto& change:scene.value("m2InstanceChanges",json::array())) {
            if(change.at("frame").get<int>()!=frame)continue;
            const auto& placement=placements.at(change.at("instance").get<size_t>());
            if(placement.wmo)throw std::runtime_error("m2InstanceChanges addresses a WMO placement");
            LOG_INFO("scene_render: applying M2 instance change at frame ",frame," for ",placement.instanceId);
            auto& models=*renderer.getM2Renderer();
            if(change.contains("hidden"))models.setInstanceHidden(placement.instanceId,change["hidden"].get<bool>());
            if(change.contains("interiorAmbient"))models.setInteriorLight(placement.instanceId,rendering::InteriorLight{
                vec3(change.at("interiorAmbient")),vec3(change.at("interiorDirect"))});
        }
        environment.realmSeconds = realmTime + frame * static_cast<double>(kStep);
        renderer.setWorldEnvironment(environment);
        for (MovingCharacter& c : movingCharacters) {
            if (frame > 0) c.position += c.velocity * kStep;
            renderer.getCharacterRenderer()->setInstancePosition(c.instance, c.position);
        }
        changeWmoMaterials(false);
        timer.call("rendererUpdate", [&] { renderer.update(kStep); });
        if (scene.value("discardFirstExtraction", false) && frame == 0) (void)renderer.extractFrameInputs();
        auto inputs = renderer.extractFrameInputs();
        changeWmoMaterials(true);
        // Isolated cloud fixtures use the same captured inputs and GPU passes
        // as the client. White-cloud captures and a matching cloud-free
        // underlay expose coverage independently of the zone palette.
        if (scene.contains("cloudProbe")) {
            const auto& probe = scene["cloudProbe"];
            auto captured = std::make_shared<rendering::FrameScene>(*inputs.scene);
            captured->sky.cloudOffset = probe.value("phase", 0.0f);
            captured->sky.flare = false;
            inputs.scene = std::move(captured);
            if (probe.contains("density"))
                inputs.weatherLight.cloudDensity = probe["density"].get<float>();
            if (probe.value("mask", false)) {
                auto& light = inputs.weatherLight;
                light.skyTopColor = light.skyMiddleColor = light.skyBand1Color =
                    light.skyBand2Color = light.skySmogColor = light.fogColor = glm::vec3(0);
                light.sunColor = light.cloudSunlitColor = light.cloudFillColor = glm::vec3(0);
                light.cloudBaseColor = glm::vec3(1);
                light.horizonGlow = light.fogDensity = 0.0f;
            }
        }

        if (interfaceFixture) {
            interfaceFixture->build();
            inputs.interface = interfaceFixture->extract();
            if (frame == scene.value("mutateAtFrame", frames - 1)) {
                if (scene.value("mutateInterfaceAfterExtract", false)) {
                    interfaceFixture->build(true);
                    (void)interfaceFixture->extract(); // grow and replace the live atlas page
                }
                if (scene.value("destroyInterfaceAfterExtract", false)) interfaceFixture.reset();
            }
        }
        if (scene.value("removeSceneAfterExtract", false) &&
            frame == scene.value("mutateAtFrame", frames - 1)) {
            for (const auto& placement : placements) {
                if (placement.wmo) renderer.scene().worldModels().removeInstance(placement.instanceId);
                else renderer.scene().models().removeInstance(placement.instanceId);
            }
            for (const auto& c : movingCharacters) renderer.scene().characters().removeInstance(c.instance);
            renderer.applySceneCommands();
            for (const auto& placement : placements) {
                if (placement.wmo) renderer.getWMORenderer()->unloadModel(placement.modelId);
                else renderer.getM2Renderer()->unloadModel(placement.modelId);
            }
            renderer.getCharacterRenderer()->clear();
            renderer.getTerrainRenderer()->removeTile(0, 0);
            renderer.liquidScene().clear();
            renderer.clearSelectionCircle();
        }
        if (scene.value("mutateAfterExtract", false) &&
            frame == scene.value("mutateAtFrame", frames - 1)) {
            // Regression: the next game frame must not change a frame that
            // was already extracted, including RT, shadows, sky and weather.
            renderer.getCamera()->setPosition(eye + glm::vec3(800, 700, 200));
            renderer.getCamera()->setRotation(170, -40);
            renderer.getCharacterPosition() += glm::vec3(600, 500, 100);
            auto next = environment;
            next.ghost = !next.ghost;
            renderer.setWorldEnvironment(next);
            renderer.getLightingManager()->setTimeOfDay(2.0f / 24.0f);
            renderer.setVolumetricFogDensity(0.0f);
            renderer.setRtLightingMode(0);
            renderer.setViewDistance(80.0f);
            renderer.setShadowDistance(40.0f);
            renderer.setWorldInView(false);
            renderer.setSharpStars(false);
            if (auto* lightning = renderer.getLightning()) lightning->reset();
            if (auto* post = renderer.getPostProcessPipeline()) {
                post->setFXAAEnabled(!inputs.postProcess.fxaa);
                post->setFSR2Enabled(!inputs.postProcess.fsr2);
                post->setBloomStrength(inputs.postProcess.bloom > 0 ? 0.0f : 2.0f);
                post->setBrightness(0.3f);
                post->setIntoxication(0.8f);
            }
        }
        // Keep fixture timing/mutation points exact while moving GPU recording
        // to the same owner used by the client. The native client probe tests
        // overlapping extraction and recording with the bounded queue.
        recordingWorker.submit(recordingWorker.reserve(), [&, inputs = std::move(inputs)] {
            timer.call("beginFrame", [&] { renderer.beginFrame(inputs); });
            if (renderer.getCamera()->getJitter() != glm::vec2(0.0f)) {
                LOG_ERROR("scene_render: recording modified the live camera's jitter");
                throw std::runtime_error("Recording changed the game camera");
            }
            timer.call("renderWorld", [&] { renderer.renderWorld(); });
            timer.call("endFrame", [&] { renderer.endFrame(); });
        });
        recordingWorker.drain();
        timer.frameDone(frame, std::chrono::duration<float, std::milli>(
                                   std::chrono::steady_clock::now() - start).count(),
                        *renderer.getVkContext());
        ++frame;
    };
    while (frame < frames) drawFrame();

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
    // A repeatable strike after streaming has settled, for lighting and trail captures.
    if (scene.contains("lightning")) {
        const auto& strike = scene["lightning"];
        renderer.getLightning()->reseed(strike.value("seed", 17u));
        renderer.getLightning()->triggerStrike(origin + vec3(strike.at("start")), origin + vec3(strike.at("end")));
        const int strikeFrames = std::max(1, static_cast<int>(std::round(strike.value("age", 0.5f) / kStep)));
        for (int i = 0; i < strikeFrames; ++i) drawFrame();
    }
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
    std::printf("scene_render: WMO shadow draw calls: %u in the kept frame\n",
                renderer.getWMORenderer()->getShadowDrawCallCount());
    if (const auto* rt = renderer.getRtScene()) {
        const auto stats = rt->stats();
        std::printf("scene_render: RT scene %u meshes %u instances %llu bytes\n",
                    stats.meshes, stats.instances, static_cast<unsigned long long>(stats.gpuBytes));
    }
    if (!capture.write(outPath)) {
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
