// What the ray tracer is handed while the region it keeps moves and the world
// changes under it. After every extraction the published instances are
// exactly those RtRegion::keeps admits - with the margin for the ones already
// kept - placed where the scene last put them, and a mesh is published while,
// and only while, a published instance places it.
#include <catch_amalgamated.hpp>

#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "rendering/rt_scene.hpp"

using namespace wowee::rendering;

namespace {

using Kind = RtScene::Change::Kind;

/// The consumer's side, built from the changes alone.
struct Consumer {
    std::map<RtScene::MeshId, std::shared_ptr<const RtScene::MeshSource>> meshes;
    std::map<RtScene::InstanceId, std::pair<RtScene::MeshId, glm::mat4>> instances;
    uint64_t serial = 0;

    void take(const RtScene::Changes& changes) {
        for (const auto& c : changes) {
            if (c.serial <= serial) continue;
            serial = c.serial;
            switch (c.kind) {
            case Kind::AddMesh:
                REQUIRE_FALSE(meshes.contains(c.id));
                meshes[c.id] = c.source;
                break;
            case Kind::RemoveMesh:
                REQUIRE(meshes.contains(c.id));
                for (const auto& [id, placed] : instances) REQUIRE(placed.first != c.id);
                meshes.erase(c.id);
                break;
            case Kind::AddInstance:
                REQUIRE_FALSE(instances.contains(c.id));
                REQUIRE(meshes.contains(c.mesh));
                instances[c.id] = {c.mesh, c.transform};
                break;
            case Kind::MoveInstance:
                REQUIRE(instances.contains(c.id));
                instances[c.id].second = c.transform;
                break;
            case Kind::RemoveInstance:
                REQUIRE(instances.contains(c.id));
                instances.erase(c.id);
                break;
            }
        }
    }
};

/// The test's own account of the world it handed the scene.
struct World {
    struct Mesh {
        float extent;
        RtAabb bounds;
    };
    struct Placed {
        RtScene::MeshId mesh;
        glm::vec3 at;
        float scale;
    };
    std::map<RtScene::MeshId, Mesh> meshes;
    std::map<RtScene::InstanceId, Placed> instances;

    static RtScene::MeshSource geometry(float extent) {
        RtScene::MeshSource source;
        source.positions = {{-extent, -extent, 0}, {extent, -extent, 0}, {0, extent, extent * 0.5f}};
        source.indices = {0, 1, 2};
        source.surfaces = {packRtSurface(glm::vec3(0.5f), 1.0f)};
        return source;
    }
    static glm::mat4 transform(const Placed& p) {
        return glm::scale(glm::translate(glm::mat4(1), p.at), glm::vec3(p.scale));
    }
    RtAabb bounds(const Placed& p) const {
        const RtAabb& b = meshes.at(p.mesh).bounds;
        return {b.bmin * p.scale + p.at, b.bmax * p.scale + p.at};
    }

    RtScene::MeshId addMesh(RtScene& scene, float extent) {
        const auto id = scene.addMesh(geometry(extent));
        REQUIRE(id != RtScene::kInvalid);
        RtAabb bounds;
        for (const auto& p : geometry(extent).positions) bounds.grow(p);
        meshes[id] = {extent, bounds};
        return id;
    }
    void place(RtScene& scene, RtScene::MeshId mesh, glm::vec3 at, float scale) {
        const Placed placed{mesh, at, scale};
        const auto id = scene.addInstance(mesh, transform(placed));
        REQUIRE(id != RtScene::kInvalid);
        instances[id] = placed;
    }
};

} // namespace

TEST_CASE("RT publishes exactly the instances its region keeps as the camera and world move",
          "[rt_region]") {
    std::mt19937 rng(20261102);
    auto uniform = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    auto chance = [&](float p) { return uniform(0, 1) < p; };
    auto pick = [&](const auto& map) {
        auto it = map.begin();
        std::advance(it, std::uniform_int_distribution<size_t>(0, map.size() - 1)(rng));
        return it->first;
    };
    constexpr float kWorld = 3000;
    auto anywhere = [&] { return glm::vec3(uniform(-kWorld, kWorld), uniform(-kWorld, kWorld), uniform(-40, 200)); };

    RtScene scene;
    World world;
    Consumer consumer;
    // Small doodads, houses, and buildings that span many hundred yards.
    for (float extent : {1.0f, 2.0f, 6.0f, 15.0f, 40.0f, 400.0f}) world.addMesh(scene, extent);
    for (int i = 0; i < 1500; ++i) {
        const auto mesh = pick(world.meshes);
        world.place(scene, mesh, anywhere(), world.meshes[mesh].extent > 100 ? 1.0f : uniform(0.5f, 2.0f));
    }

    RtRegion region;
    region.centre = {0, 0, 50};
    region.distance = 300;
    float sunYaw = 0.3f, sunPitch = 0.6f;
    std::set<RtScene::InstanceId> kept;
    uint32_t admittedTotal = 0, regionMoves = 0;

    for (int step = 0; step < 240; ++step) {
        // The camera walks, now and then jumps; the sun turns and sets.
        if (chance(0.85f)) {
            ++regionMoves;
            region.centre += glm::vec3(uniform(-40, 40), uniform(-40, 40), uniform(-5, 5));
            if (chance(0.05f)) region.centre = anywhere();
            sunYaw += uniform(0, 0.05f);
            sunPitch = glm::clamp(sunPitch + uniform(-0.08f, 0.08f), -0.3f, 1.4f);
            region.sunDirection = {std::cos(sunPitch) * std::cos(sunYaw), std::cos(sunPitch) * std::sin(sunYaw),
                                   std::sin(sunPitch)};
            region.sunlight = region.sunDirection.z > -0.05f;
            if (chance(0.05f)) region.distance = chance(0.5f) ? 150.0f : 300.0f;
            if (chance(0.05f)) region.indirectLength = chance(0.5f) ? kRtAoRadius : kRtGiLength;
            if (chance(0.03f)) region.enabled = !region.enabled;
        }
        // The world changes under it: things move, leave, arrive.
        if (chance(0.6f)) {
            for (int n = 0; n < 6; ++n) {
                const auto id = pick(world.instances);
                auto& placed = world.instances[id];
                placed.at = chance(0.2f) ? anywhere() : placed.at + glm::vec3(uniform(-30, 30), uniform(-30, 30), 0);
                scene.setInstanceTransform(id, World::transform(placed));
            }
            for (int n = 0; n < 3; ++n) {
                const auto id = pick(world.instances);
                scene.removeInstance(id);
                world.instances.erase(id);
            }
            for (int n = 0; n < 3; ++n) {
                // Often near the camera, where it is admitted at once.
                const glm::vec3 at = chance(0.5f) ? region.centre + glm::vec3(uniform(-200, 200), uniform(-200, 200), 0)
                                                  : anywhere();
                world.place(scene, pick(world.meshes), at, 1.0f);
            }
        }
        // A model is reloaded: its placements go, then it, then it comes back
        // as new geometry under an id that may be the old one.
        if (chance(0.05f)) {
            const auto mesh = pick(world.meshes);
            std::vector<glm::vec3> places;
            std::erase_if(world.instances, [&](const auto& entry) {
                if (entry.second.mesh != mesh) return false;
                scene.removeInstance(entry.first);
                places.push_back(entry.second.at);
                return true;
            });
            const float extent = world.meshes[mesh].extent * 1.01f;
            scene.removeMesh(mesh);
            world.meshes.erase(mesh);
            const auto reloaded = world.addMesh(scene, extent);
            for (const auto& at : places) world.place(scene, reloaded, at, 1.0f);
        }

        consumer.take(scene.extractChanges(region));

        std::set<RtScene::InstanceId> admitted;
        for (const auto& [id, placed] : world.instances)
            if (region.keeps(world.bounds(placed), kept.contains(id))) admitted.insert(id);
        kept = admitted;
        admittedTotal += static_cast<uint32_t>(admitted.size());

        std::set<RtScene::InstanceId> published;
        std::set<RtScene::MeshId> placing;
        for (const auto& [id, placed] : consumer.instances) {
            published.insert(id);
            placing.insert(placed.first);
            const auto& wanted = world.instances.at(id);
            CHECK(placed.first == wanted.mesh);
            CHECK(placed.second == World::transform(wanted));
        }
        INFO("step " << step);
        REQUIRE(published == admitted);
        std::set<RtScene::MeshId> meshes;
        for (const auto& [id, source] : consumer.meshes) {
            meshes.insert(id);
            CHECK(source->positions == World::geometry(world.meshes.at(id).extent).positions);
        }
        REQUIRE(meshes == placing);
    }
    // The walk saw the region move and admit and drop a real share of the world.
    CHECK(regionMoves > 150);
    CHECK(admittedTotal > 240 * 30);
}
