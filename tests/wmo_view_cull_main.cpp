// Which groups of a building each colour pass of the WMO renderer draws:
// the camera's view and the water's reflection, each from its own eye. The
// cull runs on the GPU (wmo_cull.comp); what it drew is read back from its
// counts. Each wall draws a number of ranges no other sum of walls makes,
// so a count names the walls drawn. Requires Vulkan and WotLK data; exits
// 77 without a device.
#include "asset_tables.hpp"
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
#include <optional>
#include <span>
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

// A wall four yards square facing -X, standing at `centre`, of `strips`
// upright strips, each a batch of its own and so a range drawn of its own.
pipeline::WMOGroup wall(glm::vec3 centre, uint32_t strips) {
    pipeline::WMOGroup group{};
    group.flags = 0x8;  // exterior
    group.boundingBoxMin = centre - glm::vec3(0.5f, 2.0f, 2.0f);
    group.boundingBoxMax = centre + glm::vec3(0.5f, 2.0f, 2.0f);
    const float width = 4.0f / static_cast<float>(strips);
    for (uint32_t strip = 0; strip < strips; ++strip) {
        const float left = -2.0f + width * static_cast<float>(strip);
        const auto first = static_cast<uint16_t>(group.vertices.size());
        for (glm::vec2 corner : {glm::vec2(left, -2), glm::vec2(left + width, -2), glm::vec2(left + width, 2),
                                 glm::vec2(left, 2)}) {
            pipeline::WMOVertex vertex{};
            vertex.position = centre + glm::vec3(0.0f, corner.x, corner.y);
            vertex.normal = {-1, 0, 0};
            vertex.color = glm::vec4(1.0f);
            group.vertices.push_back(vertex);
        }
        pipeline::WMOBatch batch{};
        batch.startIndex = static_cast<uint32_t>(group.indices.size());
        batch.indexCount = 6;
        batch.startVertex = first;
        batch.lastVertex = static_cast<uint16_t>(first + 3);
        for (uint16_t i : {0, 1, 2, 0, 2, 3}) group.indices.push_back(static_cast<uint16_t>(first + i));
        group.batches.push_back(batch);
    }
    return group;
}

// Three walls of one building, by where they stand from a camera at the
// origin looking along +X: ahead of it, behind it, and high up ahead; and
// the ranges each draws.
const glm::vec3 kAhead{60, 0, 2}, kBehind{-60, 0, 2}, kHigh{60, 0, 40};
constexpr uint32_t kAheadRanges = 1, kBehindRanges = 2, kHighRanges = 4;

pipeline::WMOModel building() {
    pipeline::WMOModel model{};
    model.groups = {wall(kAhead, kAheadRanges), wall(kBehind, kBehindRanges), wall(kHigh, kHighRanges)};
    model.nGroups = static_cast<uint32_t>(model.groups.size());
    model.materials.push_back(pipeline::WMOMaterial{});
    model.boundingBoxMin = glm::vec3(-61, -2, 0);
    model.boundingBoxMax = glm::vec3(61, 2, 42);
    return model;
}

// The walls a count of ranges drawn names.
std::string walls(uint32_t ranges) {
    std::string out;
    const auto add = [&](uint32_t bit, const char* name) {
        if (ranges & bit) out += (out.empty() ? "" : ", ") + std::string(name);
    };
    add(kAheadRanges, "ahead");
    add(kBehindRanges, "behind");
    add(kHighRanges, "high");
    return "{" + out + "}";
}

void expect(uint32_t got, uint32_t want, const std::string& what) {
    require(got == want, what + ": drew " + walls(got) + ", expected " + walls(want));
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

    // A wide pool in front of the camera, where both walls ahead are mirrored.
    rendering::ReflectedCamera eye;
    eye.position = camera.getPosition();
    eye.forward = camera.getForward();
    eye.view = camera.getViewMatrix();
    eye.projection = camera.getProjectionMatrix();
    const rendering::WaterInView pool{glm::vec3(5, -60, -0.2f), glm::vec3(150, 60, 0.2f), 0.0f};
    const auto mirror = rendering::waterReflectionView(eye, std::span(&pool, 1));
    require(mirror.has_value(), "a camera above the pool has a reflection");

    // A frame culled on the GPU; its counts are read when its slot is
    // prepared again.
    auto& context = *renderer.getVkContext();
    wmo.useFrame(wmo.extract(camera, camera.getPosition(), &*mirror));
    for (int pass = 0; pass < 2; ++pass) {
        context.immediateSubmit([&](VkCommandBuffer cmd) { wmo.prepareDraws(cmd, 0, VK_NULL_HANDLE, 0); });
        wmo.frameSubmitted();
    }
    const auto& counted = wmo.cullCounts();
    expect(counted.viewDraws, kAheadRanges, "the view");
    expect(counted.reflectionDraws, kAheadRanges | kHighRanges, "the reflection");
    require(counted.frustumCulled == 2, "the view counts the two walls it cannot see, not " +
                                        std::to_string(counted.frustumCulled));
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
        std::optional<test::AssetTables> tables;
        rendering::Renderer renderer;
        try {
            require(surface.context.initialize(window), "Vulkan initialization failed");
            const char* root = std::getenv("WOW_DATA_PATH");
            const std::string data = root ? root : "./Data";
            assets.setExpansionDataPath(data + "/expansions/wotlk");
            require(assets.initialize(data + "/expansions/wotlk"), "WotLK assets unavailable");
            assets.setBaseFallbackPath(data, "wotlk");
            renderer.setGameTables(tables.emplace(assets, data));
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
