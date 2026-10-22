// Public renderer APIs -> real RtScene handoff. Requires Vulkan and WotLK data.
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "rendering/render_surface.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_context.hpp"
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace wowee;
namespace {
void require(bool ok, const char* message) {
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
pipeline::M2Model triangle() {
    pipeline::M2Model model{};
    model.name = "rt-placement-fixture.m2";
    for (glm::vec3 position : {glm::vec3(-5,-5,0), glm::vec3(5,-5,0), glm::vec3(0,5,10)}) {
        pipeline::M2Vertex vertex{}; vertex.position = position; vertex.normal = {0,0,1};
        model.vertices.push_back(vertex);
    }
    model.indices = {0,1,2}; model.materials.push_back({0,0});
    pipeline::M2Batch batch{}; batch.indexCount = batch.vertexCount = 3;
    batch.colorIndex = batch.transparencyIndex = batch.textureAnimIndex = 0xffff;
    model.batches.push_back(batch);
    model.boundMin = {-5,-5,0}; model.boundMax = {5,5,10}; model.boundRadius = 10;
    return model;
}
void exercise(rendering::Renderer& renderer, pipeline::AssetManager& assets) {
    auto& m2 = *renderer.getM2Renderer();
    auto& wmo = *renderer.getWMORenderer();
    auto& rt = *renderer.getRtScene();
    const auto model = triangle();
    require(m2.loadModel(model, 100), "load M2 fixture");
    const std::string path = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
    auto ship = pipeline::WMOLoader::load(assets.readFile(path));
    require(ship.nGroups > 0, "load WMO fixture");
    for (uint32_t g = 0; g < ship.nGroups; ++g)
        require(pipeline::WMOLoader::loadGroup(assets.readFile(pipeline::wmoGroupCandidates(path, g).front()), ship, g), "load WMO group");
    require(wmo.loadModel(ship, 200), "upload WMO fixture");
    const glm::vec3 origin(6000,-500,650);
    auto first = m2.createInstance(100, origin, {}, 1, false);
    auto child = m2.createInstance(100, origin + glm::vec3(4,0,0), {}, 1, false);
    auto hull = wmo.createInstance(200, origin, {}, 1);
    require(first && child && hull, "create placements");
    rendering::Camera facing;
    facing.setPosition(origin+glm::vec3(400,0,80));
    facing.setRotation(180,-10);
    auto away=facing;
    away.setRotation(0,0);
    const auto reflected=rendering::SceneView::fromCamera(away);
    const auto views=wmo.extract(facing,facing.getPosition(),{},&reflected);
    require(views->view.used>0,"front view lost the ship");
    require(views->reflection.used==0,"reflection cull used the primary camera");
    require(views->reflection.frustumCulled>0,"opposite view did not reject offscreen groups");
    require(views->depth.used>0,"primary cull removed shadow casters");
    const auto reversed=wmo.extract(away,away.getPosition(),{});
    require(reversed->view.used==0,"camera pointing away still draws ship groups");
    require(views->view.used>0,"next extraction mutated the queued view");
    wmo.setM2Renderer(&m2);
    wmo.addDoodadToInstance(hull, child, glm::translate(glm::mat4(1), glm::vec3(4,0,0)));
    auto extract = [&] {
        m2.syncRtScene(); wmo.syncRtScene(); return rt.extractChanges();
    };
    auto accept = [&](const rendering::RtScene::Changes& changes, uint32_t expected) {
        rt.applyChanges(changes);
        require(rt.stats().instances == expected, "RT placement count differs from visible scene");
    };
    accept(extract(), 3);
    require(extract().empty(), "stationary frame published RT edits");

    // A moved transport moves its child; multiple edits coalesce to the last.
    m2.setInstancePosition(first, origin + glm::vec3(1,0,0));
    m2.setInstanceTransform(first, glm::translate(glm::mat4(1), origin + glm::vec3(9,0,0)));
    wmo.setInstanceTransform(hull, glm::translate(glm::mat4(1), origin + glm::vec3(20,0,0)));
    auto moved = extract();
    require(std::count_if(moved.begin(), moved.end(), [](const auto& c) {
        return c.kind == rendering::RtScene::Change::Kind::MoveInstance;
    }) == 3, "moving a hull and two M2s did not publish three final transforms");
    for (float dx : {9.f,20.f,24.f})
        require(std::any_of(moved.begin(), moved.end(), [&](const auto& c) {
            return glm::vec3(c.transform[3]) == origin + glm::vec3(dx,0,0);
        }), "final RT transform missing");
    accept(moved, 3);

    m2.setInstanceFade(first, .5f); wmo.setInstanceHidden(hull, true);
    accept(extract(), 0);
    m2.setInstanceFade(first, 1); wmo.setInstanceHidden(hull, false);
    accept(extract(), 3);

    // RT disabled while mutations accumulate; enabling sees final state.
    rendering::RtRegion off; off.enabled = false;
    accept(rt.extractChanges(off), 0);
    m2.setInstanceHidden(first, true);
    wmo.setInstanceTransform(hull, glm::translate(glm::mat4(1), origin + glm::vec3(30,0,0)));
    accept(extract(), 2);
    m2.setInstanceHidden(first, false); accept(extract(), 3);

    // Drop an extracted frame: subsequent deltas must still carry its changes.
    m2.setInstancePosition(first, origin + glm::vec3(40,0,0));
    const auto dropped = extract();
    require(!dropped.empty(), "moved instance missing from dropped frame");
    m2.removeInstances({first, child}); wmo.removeInstance(hull);
    accept(extract(), 0);
    require(glm::vec3(dropped.back().transform[3]) == origin + glm::vec3(40,0,0), "retained delta was mutated");

    // Mesh/id reuse and a newly loaded model after a quiet frame.
    m2.unloadModel(100); wmo.unloadModel(200); accept(extract(), 0);
    require(m2.loadModel(model, 100), "reload M2 fixture");
    require(wmo.loadModel(ship, 200), "reload WMO fixture");
    first = m2.createInstance(100, origin, {}, 1, false);
    hull = wmo.createInstance(200, origin, {}, 1);
    accept(extract(), 2);
    {
        rendering::M2Renderer::InstanceSession m2Session(m2);
        rendering::WMORenderer::InstanceSession wmoSession(wmo);
        m2.setInstancePosition(first, origin + glm::vec3(50,0,0));
        accept(extract(), 2);
    }
    accept(extract(), 0);
    require(extract().empty(), "scene teardown left pending RT changes");
    std::puts("PASS: renderer RT mutations, transport children, disabled RT, dropped frames and session teardown");
}
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
    auto* window = SDL_CreateWindow("RT changes test", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
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
            exercise(renderer, assets);
            result = 0;
        } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); }
        renderer.shutdown();
    }
    SDL_DestroyWindow(window); SDL_Quit();
    return result;
}
