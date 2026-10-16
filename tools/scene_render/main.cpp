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
//     "weather": "none",                  // none | rain | snow | storm
//     "weatherIntensity": 0.0,
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
//         "scale": 1.0 }
//     ],
//     "camera": { "offset": [-8, 0, 3],   // from the origin; or "position"
//                 "lookAt": [0, 0, 2] },  // from the origin; or "yaw"/"pitch"
//     "fov": 60.0,
//     "size": [1280, 720],
//     "frames": 30,                       // drawn before the one that is kept
//     "shadowQuality": 2,                 // 0-4, as the settings slider
//     "minimap": false                    // the minimap disc in the corner
//   }
//
// Without a camera it backs off along -X far enough to see the first model.
// Render axes: z is up, yaw 0 looks along +x and 90 along +y.

#include "core/coordinates.hpp"
#include "rendering/minimap.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/m2_renderer.hpp"
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
    return true;
}

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool valid = false;
};

/// Load one model through the client's loaders and place it. Its bounds,
/// in world space, extend `bounds`.
bool placeModel(const json& entry, const glm::vec3& origin, pipeline::AssetManager& assets,
                rendering::Renderer& renderer, uint32_t modelId, Bounds& bounds) {
    const std::string path = entry.value("path", "");
    const glm::vec3 position = origin + vec3(entry.value("offset", json()));
    const glm::vec3 rotation = glm::radians(vec3(entry.value("rotation", json())));
    const float scale = entry.value("scale", 1.0f);
    glm::vec3 lo(0.0f), hi(0.0f);
    const std::string lower = lowered(path);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".wmo") == 0) {
        auto wmo = pipeline::WMOLoader::load(assets.readFile(path));
        for (uint32_t group = 0; group < wmo.nGroups; ++group) {
            const auto candidates = pipeline::wmoGroupCandidates(path, group);
            if (!pipeline::WMOLoader::loadGroup(assets.readFile(candidates.front()), wmo, group)) {
                LOG_ERROR("scene_render: cannot load group ", group, " of ", path);
                return false;
            }
        }
        if (!renderer.getWMORenderer()->loadModel(wmo, modelId)) return false;
        renderer.getWMORenderer()->createInstance(modelId, position, rotation, scale);
        lo = wmo.boundingBoxMin;
        hi = wmo.boundingBoxMax;
    } else {
        auto model = pipeline::M2Loader::load(assets.readFile(path));
        model.name = path;
        if (!pipeline::M2Loader::loadSkin(assets.readFile(pipeline::skinPathForM2(path)), model)) {
            LOG_ERROR("scene_render: cannot load the skin of ", path);
            return false;
        }
        if (!renderer.getM2Renderer()->loadModel(model, modelId)) return false;
        renderer.getM2Renderer()->createInstance(modelId, position, rotation, scale, false);
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
    LOG_INFO("scene_render: placed ", path);
    return true;
}

uint32_t weatherType(const std::string& name) {
    if (name == "rain") return 1;
    if (name == "snow") return 2;
    if (name == "storm") return 3;
    return 0;
}

int render(const json& scene, const std::string& outPath, pipeline::AssetManager& assets,
           rendering::Renderer& renderer) {
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
    for (const auto& entry : scene.value("models", json::array())) {
        if (!placeModel(entry, origin, assets, renderer, modelId++, bounds)) return 1;
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
    environment.weatherType = weatherType(scene.value("weather", std::string("none")));
    environment.weatherIntensity = scene.value("weatherIntensity", 0.0f);
    environment.raining = environment.weatherType == 1 || environment.weatherType == 3;
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

    // Fixed steps, so the same scene draws the same frame. Terrain streams in
    // on its own threads: with it on, the frames wait for it as well.
    const int frames = std::max(1, scene.value("frames", 30));
    constexpr float kStep = 1.0f / 30.0f;
    const auto* terrain = renderer.getTerrainManager();
    const bool waitTerrain = scene.value("terrain", false) && terrain;
    for (int frame = 0, extra = 0; frame < frames || (waitTerrain && extra < 600 &&
                                                       terrain->getRemainingTileCount() > 0);
         ++frame) {
        if (frame >= frames) ++extra;
        environment.realmSeconds = realmTime + frame * static_cast<double>(kStep);
        renderer.setWorldEnvironment(environment);
        renderer.update(kStep);
        renderer.beginFrame();
        renderer.renderWorld();
        renderer.endFrame();
    }
    if (!renderer.captureScreenshot(outPath)) {
        std::fprintf(stderr, "scene_render: could not write %s\n", outPath.c_str());
        return 1;
    }
    return 0;
}

int run(const json& scene, const std::string& outPath) {
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
                status = render(scene, outPath, assets, renderer);
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
    if (argc != 3) {
        std::fprintf(stderr, "usage: scene_render <scene.json> <out.png>\n");
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
    return run(scene, argv[2]);
}
