#include <catch_amalgamated.hpp>
#include "rendering/rt_instance_changes.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <utility>

using wowee::rendering::RtInstanceChanges;
namespace {
struct Sink {
    uint32_t next = 1, adds = 0, moves = 0, removes = 0;
    bool fail = false;
    std::unordered_map<uint32_t, RtInstanceChanges::State> drawn;
    uint32_t addInstance(uint32_t mesh, const glm::mat4& transform) {
        ++adds;
        if (fail) return RtInstanceChanges::invalid;
        drawn.emplace(next, RtInstanceChanges::State{mesh, transform});
        return next++;
    }
    void setInstanceTransform(uint32_t id, const glm::mat4& matrix) { ++moves; drawn.at(id).transform = matrix; }
    void removeInstance(uint32_t id) { ++removes; REQUIRE(drawn.erase(id) == 1); }
};
struct Scene {
    RtInstanceChanges changes;
    Sink sink;
    size_t reads = 0, failures = 0;
    std::unordered_map<uint32_t, RtInstanceChanges::State> instances;
    void add(uint32_t id, uint32_t model, uint32_t mesh) {
        instances[id] = {mesh, glm::mat4(1)};
        changes.add(id, model);
    }
    void flush() {
        changes.flush(sink, [&](uint32_t id) { ++reads; return instances.at(id); },
                      [&](uint32_t, uint32_t) { ++failures; });
    }
    void remove(uint32_t id) { changes.remove(id, &sink); instances.erase(id); }
};
}

TEST_CASE("stationary RT placements require no scene reads or sink updates", "[rt-changes]") {
    Scene scene;
    for (uint32_t id = 1; id <= 1000; ++id) scene.add(id, id % 8, 42);
    scene.flush();
    REQUIRE(scene.sink.drawn.size() == 1000);
    scene.reads = 0;
    for (int frame = 0; frame < 20; ++frame) scene.flush();
    CHECK(scene.reads == 0);
    CHECK(scene.sink.moves == 0);
}

TEST_CASE("several mutations publish only the final transform of the changed placement", "[rt-changes]") {
    Scene scene;
    scene.add(1, 10, 42); scene.add(2, 10, 42); scene.flush();
    scene.reads = 0;
    for (float x : {1.f, 2.f, 9.f}) {
        scene.instances.at(2).transform = glm::translate(glm::mat4(1), glm::vec3(x, 3, 4));
        scene.changes.changed(2);
    }
    scene.flush();
    CHECK(scene.reads == 1);
    CHECK(scene.sink.moves == 1);
    CHECK(std::any_of(scene.sink.drawn.begin(), scene.sink.drawn.end(), [](const auto& entry) {
        return glm::vec3(entry.second.transform[3]) == glm::vec3(9, 3, 4);
    }));
}

TEST_CASE("hidden or faded placements leave RT and can return without moving", "[rt-changes]") {
    Scene scene; scene.add(1, 10, 42); scene.flush();
    scene.instances.at(1).mesh = RtInstanceChanges::invalid;
    scene.changes.changed(1); scene.flush();
    CHECK(scene.sink.drawn.empty());
    scene.instances.at(1).mesh = 42;
    scene.changes.changed(1); scene.flush();
    CHECK(scene.sink.drawn.size() == 1);
    CHECK(scene.sink.adds == 2);
}

TEST_CASE("model replacement retires placements before the mesh and wakes only its users", "[rt-changes]") {
    Scene scene;
    scene.add(1, 10, 42); scene.add(2, 10, 42); scene.add(3, 11, 99); scene.flush();
    scene.changes.modelRemoved(10, &scene.sink);
    REQUIRE(scene.sink.drawn.size() == 1);
    CHECK(scene.sink.drawn.begin()->second.mesh == 99);
    scene.instances.at(1).mesh = scene.instances.at(2).mesh = RtInstanceChanges::invalid;
    scene.flush(); // model is absent for a frame
    scene.instances.at(1).mesh = scene.instances.at(2).mesh = 77;
    scene.changes.modelReady(10);
    scene.reads = 0; scene.flush();
    CHECK(scene.reads == 2);
    CHECK(scene.sink.drawn.size() == 3);
}

TEST_CASE("removal while dirty and reused ids do not address a retired placement", "[rt-changes]") {
    Scene scene; scene.add(1, 10, 42); scene.add(2, 10, 42); scene.flush();
    scene.changes.changed(1); scene.remove(1);
    scene.add(1, 11, 99); scene.remove(2); scene.flush();
    REQUIRE(scene.sink.drawn.size() == 1);
    CHECK(scene.sink.drawn.begin()->second.mesh == 99);
    scene.add(3, 11, 99); scene.remove(3); scene.flush();
    CHECK(scene.sink.drawn.size() == 1);
    auto moved = std::move(scene.changes);
    moved.clear(&scene.sink);
    CHECK(scene.sink.drawn.empty());
}

TEST_CASE("unavailable meshes wait for publication and failures do not retry every frame", "[rt-changes]") {
    Scene scene; scene.add(1, 10, RtInstanceChanges::invalid); scene.flush();
    CHECK(scene.sink.adds == 0);
    scene.instances.at(1).mesh = 42; scene.changes.modelReady(10);
    scene.sink.fail = true; scene.flush();
    CHECK(scene.failures == 1);
    for (int frame = 0; frame < 10; ++frame) scene.flush();
    CHECK(scene.failures == 1);
    scene.sink.fail = false; scene.changes.modelReady(10); scene.flush();
    CHECK(scene.sink.drawn.size() == 1);
}
