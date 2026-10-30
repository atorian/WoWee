// Which groups of a building each colour pass of the WMO renderer draws:
// the camera's view and the water's reflection, each from its own eye.
// Requires Vulkan and WotLK data; exits 77 without a device.
#include "pipeline/asset_manager.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/camera.hpp"
#include "rendering/render_surface.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/water_reflection.hpp"
#include "rendering/wmo_renderer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>

using namespace wowee;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
class Surface : public rendering::RenderSurface {
public:
    mutable rendering::VkContext context;
    ~Surface() override { context.shutdown(); }
    rendering::VkContext* getVkContext() const override { return &context; }
    int getWidth() const override { return 320; }
    int getHeight() const override { return 240; }
    int getDrawableWidth() const override { return 320; }
    int getDrawableHeight() const override { return 240; }
};

// A wall four yards square facing -X, standing at `centre`.
pipeline::WMOGroup wall(glm::vec3 centre) {
    pipeline::WMOGroup group{};
    group.flags = 0x8;  // exterior
    group.boundingBoxMin = centre - glm::vec3(0.5f, 2.0f, 2.0f);
    group.boundingBoxMax = centre + glm::vec3(0.5f, 2.0f, 2.0f);
    for (glm::vec2 corner : {glm::vec2(-2, -2), glm::vec2(2, -2), glm::vec2(2, 2), glm::vec2(-2, 2)}) {
        pipeline::WMOVertex vertex{};
        vertex.position = centre + glm::vec3(0.0f, corner.x, corner.y);
        vertex.normal = {-1, 0, 0};
        vertex.color = glm::vec4(1.0f);
        group.vertices.push_back(vertex);
    }
    group.indices = {0, 1, 2, 0, 2, 3};
    pipeline::WMOBatch batch{};
    batch.indexCount = 6;
    batch.lastVertex = 3;
    group.batches.push_back(batch);
    return group;
}

// Three walls of one building, by where they stand from a camera at the
// origin looking along +X: ahead of it, behind it, and high up ahead.
const glm::vec3 kAhead{60, 0, 2}, kBehind{-60, 0, 2}, kHigh{60, 0, 40};

pipeline::WMOModel building() {
    pipeline::WMOModel model{};
    model.groups = {wall(kAhead), wall(kBehind), wall(kHigh)};
    model.nGroups = static_cast<uint32_t>(model.groups.size());
    model.materials.push_back(pipeline::WMOMaterial{});
    model.boundingBoxMin = glm::vec3(-61, -2, 0);
    model.boundingBoxMax = glm::vec3(61, 2, 42);
    return model;
}

// The walls a pass's list draws, by where they stand.
template <typename List>
std::set<std::string> walls(const List& list) {
    std::set<std::string> out;
    for (const auto& entry : list.drawn()) {
        for (const auto& group : entry.groups) {
            const glm::vec3 centre = (group.worldMin + group.worldMax) * 0.5f;
            if (glm::distance(centre, kAhead) < 1.0f) out.insert("ahead");
            else if (glm::distance(centre, kBehind) < 1.0f) out.insert("behind");
            else if (glm::distance(centre, kHigh) < 1.0f) out.insert("high");
            else out.insert("unknown");
        }
    }
    return out;
}

std::string named(const std::set<std::string>& names) {
    std::string out = "{";
    for (const auto& name : names) out += (out.size() > 1 ? ", " : "") + name;
    return out + "}";
}

void expect(const std::set<std::string>& got, const std::set<std::string>& want, const std::string& what) {
    require(got == want, what + ": drew " + named(got) + ", expected " + named(want));
}

void exercise(rendering::Renderer& renderer) {
    auto& wmo = *renderer.getWMORenderer();
    require(wmo.loadModel(building(), 300), "upload WMO fixture");
    require(wmo.createInstance(300, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f) != 0, "place WMO fixture");

    // Ten yards above still water at zero, looking along +X and down a
    // little: the high wall is above the top of the view, and the water
    // shows it.
    rendering::Camera camera;
    camera.setPosition({0, 0, 10});
    camera.setRotation(0.0f, -20.0f);
    const std::vector<rendering::LocalLight> lights;

    const auto reflection = rendering::waterReflectionView(camera, 0.0f);
    require(reflection.has_value(), "a camera above the water has a reflection");
    {
        const auto frame = wmo.extract(camera, camera.getPosition(), lights,
                                       reflection->projection * reflection->view);
        expect(walls(frame->view), {"ahead"}, "the view");
        expect(walls(frame->reflection), {"ahead", "high"}, "the reflection");
        require(frame->view.frustumCulled == 2, "the view counts the two walls it cannot see, not " +
                                                std::to_string(frame->view.frustumCulled));
    }

    // Under the water there is no reflection to draw, and so nothing in its list.
    camera.setPosition({0, 0, 0.2f});
    require(!rendering::waterReflectionView(camera, 0.0f).has_value(),
            "a camera at the water's surface has no reflection");
    {
        const auto frame = wmo.extract(camera, camera.getPosition(), lights, std::nullopt);
        require(walls(frame->reflection).empty(), "no reflection, yet its list drew " +
                                                      named(walls(frame->reflection)));
    }
    std::puts("PASS: the view and the reflection each draw the WMO groups their own eye can see");
}
}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
    auto* window = SDL_CreateWindow("WMO view cull test", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) { SDL_Quit(); return 77; }
    int result = 1;
    {
        Surface surface;
        pipeline::AssetManager assets;
        rendering::Renderer renderer;
        try {
            require(surface.context.initialize(window), "Vulkan initialization failed");
            const char* root = std::getenv("WOW_DATA_PATH");
            const std::string data = root ? root : "./Data";
            assets.setExpansionDataPath(data + "/expansions/wotlk");
            require(assets.initialize(data + "/expansions/wotlk"), "WotLK assets unavailable");
            assets.setBaseFallbackPath(data, "wotlk");
            require(renderer.initialize(&surface), "renderer initialization failed");
            require(renderer.initializeRenderers(&assets, "Northrend"), "world renderers initialization failed");
            renderer.getTerrainManager()->setStreamingEnabled(false);
            exercise(renderer);
            result = 0;
        } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); }
        renderer.shutdown();
    }
    SDL_DestroyWindow(window); SDL_Quit();
    return result;
}
