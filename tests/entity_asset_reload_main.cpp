// Optional native regression: real spawns and resident GPU geometry across
// lookup invalidation, as when realm patches mount between two maps.
#include "core/entity_spawner.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/render_surface.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/wmo_renderer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace wowee;
class Surface final : public rendering::RenderSurface {
public:
    mutable rendering::VkContext context;
    rendering::VkContext* getVkContext() const override { return &context; }
    int getWidth() const override { return 320; }
    int getHeight() const override { return 240; }
    int getDrawableWidth() const override { return getWidth(); }
    int getDrawableHeight() const override { return getHeight(); }
    ~Surface() override { context.shutdown(); }
};
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

int exercise(rendering::Renderer& renderer, pipeline::AssetManager& assets) {
    std::puts("Starting world renderers");
    require(renderer.initializeRenderers(&assets, "Northrend"), "world renderers did not initialize");
    renderer.getTerrainManager()->setStreamingEnabled(false);
    game::GameServices services;
    core::EntitySpawner spawner(&renderer, &assets, nullptr, &services, nullptr);
    spawner.initialize();
    std::puts("Spawner lookups ready");
    const auto dbc = assets.loadDBC("GameObjectDisplayInfo.dbc");
    require(dbc && dbc->isLoaded(), "GameObjectDisplayInfo is not available");
    std::vector<uint32_t> displays{3015, 3031}; // ship and zeppelin, deliberately different geometry
    for (const auto* token : {"pumpkin", "sign"}) {
        uint32_t chosen = 0;
        for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
            const uint32_t id = dbc->getUInt32(row, 0);
            auto path = spawner.getGameObjectModelPathForDisplayId(id);
            std::transform(path.begin(), path.end(), path.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (path.ends_with(".m2") && path.find(token) != std::string::npos) {
                chosen = id;
                break;
            }
        }
        require(chosen != 0, std::string("fixture has no ") + token + " display");
        displays.push_back(chosen);
    }
    struct Shape { bool wmo; uint32_t triangles; };
    std::vector<Shape> reference;
    auto measure = [&](uint32_t display) {
        std::printf("Spawning display %u\n", display);
        core::JobGroup jobs;
        core::EntitySpawner::SpawnState state(jobs);
        spawner.bindMap(&state);
        struct Unbind { core::EntitySpawner& s; ~Unbind() { s.bindMap(nullptr); } } unbind{spawner};
        constexpr uint64_t guid = 1;
        spawner.queueGameObjectSpawn(guid, 0, display, 0, 0, 0, 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!spawner.getGameObjectInstances().contains(guid)) {
            require(std::chrono::steady_clock::now() < deadline, "queued spawn did not finish");
            spawner.update();
            renderer.applySceneCommands();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        renderer.applySceneCommands();
        const auto found = spawner.getGameObjectInstances().find(guid);
        require(found != spawner.getGameObjectInstances().end(), "game object was not placed");
        const auto& object = found->second;
        Shape shape{object.isWmo, object.isWmo ? renderer.getWMORenderer()->getTotalTriangleCount()
                                             : renderer.getM2Renderer()->getTotalTriangleCount()};
        std::printf("display %u, %s, model %u, %u triangles\n", display,
                    object.isWmo ? "WMO" : "M2", object.modelId, shape.triangles);
        require(shape.triangles > 0, "placed object has no geometry");
        spawner.despawnGameObject(guid);
        renderer.applySceneCommands();
        return shape;
    };
    for (const auto display : displays) reference.push_back(measure(display));
    require(reference[0].triangles != reference[1].triangles, "ship and zeppelin fixture must differ");
    require(reference[2].triangles != reference[3].triangles, "pumpkin and sign fixture must differ");

    // The GPU cache intentionally survives. Rebuilding tables must not let a
    // different asset acquire the identity of any of these resident meshes.
    spawner.rebuildLookups();
    bool passed = true;
    for (const size_t i : {size_t{1}, size_t{0}, size_t{3}, size_t{2}}) {
        const auto got = measure(displays[i]);
        if (got.wmo != reference[i].wmo || got.triangles != reference[i].triangles) {
            std::fprintf(stderr, "FAIL: display %u changed geometry after lookup rebuild: %u -> %u triangles\n",
                         displays[i], reference[i].triangles, got.triangles);
            passed = false;
        }
    }
    // Attachment models share the same lifetime rule as game objects.
    const auto weapon = spawner.allocateWeaponModelId();
    spawner.rebuildLookups();
    require(spawner.allocateWeaponModelId() != weapon, "lookup rebuild recycled a resident attachment ID");
    if (passed) std::puts("PASS: WMO, M2 and attachment identities survive lookup rebuild");
    return passed ? 0 : 1;
}
}

int main() {
    using namespace wowee;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("Entity asset reload regression");
    const char* root = std::getenv("WOW_DATA_PATH");
    if (!root || !std::filesystem::exists(std::filesystem::path(root) / "expansions/wotlk/manifest.json")) {
        std::puts("SKIP: WOW_DATA_PATH with extracted WotLK assets is required");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SKIP: SDL video unavailable: %s\n", SDL_GetError());
        return 77;
    }
    auto* window = SDL_CreateWindow("entity asset reload", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "SKIP: Vulkan window unavailable: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }
    int status = 1;
    {
        Surface surface;
        if (!surface.context.initialize(window)) status = 77;
        else {
            std::puts("Vulkan ready");
            pipeline::AssetManager assets;
            pipeline::DBCLayout layout;
            rendering::Renderer renderer;
            try {
                require(assets.initialize((std::filesystem::path(root) / "expansions/wotlk").string()), "asset initialization failed");
                require(layout.loadFromJson((std::filesystem::path(root) / "expansions/wotlk/dbc_layouts.json").string()), "DBC layouts did not load");
                pipeline::setActiveDBCLayout(&layout);
                require(renderer.initialize(&surface), "renderer initialization failed");
                std::puts("Renderer ready");
                status = exercise(renderer, assets);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "FAIL: %s\n", e.what());
            }
            renderer.shutdown();
            pipeline::setActiveDBCLayout(nullptr);
        }
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
