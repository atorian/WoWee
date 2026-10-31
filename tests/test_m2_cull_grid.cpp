/// The M2 passes throw away whole 64-yard columns before testing instances.
/// That is only safe if a column is never thrown away while something in it
/// would have passed the per-instance test - for shadows above all, where the
/// caster is often outside the camera's view and its shadow is not.

#include <catch_amalgamated.hpp>

#include "rendering/m2_cull_grid.hpp"
#include "rendering/frustum.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <random>

using wowee::rendering::cellMayCastShadow;
using wowee::rendering::distanceSqToBox;
using wowee::rendering::Frustum;
using wowee::rendering::M2CullGrid;

namespace {

// The shadow pass's per-instance test, as M2Renderer::renderShadow writes it.
bool instanceCasts(const glm::mat4& lightSpace, const glm::vec3& position, float shadowRadius,
                   float boundRadiusTimesScale) {
    const glm::vec4 clip = lightSpace * glm::vec4(position, 1.0f);
    const float margin = boundRadiusTimesScale / shadowRadius * 1.5f;
    if (std::abs(clip.x) > 1.0f + margin || std::abs(clip.y) > 1.0f + margin) return false;
    return clip.z >= -margin && clip.z <= 1.0f + margin;
}

// Files instance `index` as M2Renderer::fileInCullGrid does: its record from
// the instance, then its column.
void place(M2CullGrid& grid, uint32_t index, const glm::vec3& position, const glm::vec3& center,
           float cullRadius, float shadowRadius) {
    while (grid.size() <= index) grid.add();
    auto& record = grid.record(index);
    record.position = position;
    record.cullCenter = center;
    record.paddedRadius = cullRadius;
    record.shadowRadius = shadowRadius;
    grid.file(index);
}

// The column instance `index` is listed in, or nullptr.
const M2CullGrid::Cell* columnOf(const M2CullGrid& grid, uint32_t index) {
    const M2CullGrid::Cell* found = nullptr;
    for (const auto& cell : grid.cells()) {
        for (const uint32_t member : cell.members) {
            if (member != index) continue;
            REQUIRE(found == nullptr);  // in one column at most
            found = &cell;
        }
    }
    return found;
}

// A light box like Renderer::computeLightSpaceMatrix builds, around `center`.
glm::mat4 sunMatrix(const glm::vec3& center, glm::vec3 sunDir, float distance) {
    sunDir = glm::normalize(sunDir);
    glm::mat4 view = glm::lookAt(center - sunDir * distance * 3.0f, center, glm::vec3(0, 0, 1));
    glm::mat4 proj = glm::ortho(-distance, distance, -distance, distance, 1.0f, distance * 6.5f);
    proj[1][1] *= -1.0f;
    return proj * view;
}

}  // namespace

TEST_CASE("a column is kept whenever one of its instances would cast") {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> coord(-1500.0f, 1500.0f);
    std::uniform_real_distribution<float> height(-50.0f, 400.0f);
    std::uniform_real_distribution<float> radius(0.1f, 40.0f);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);

    for (int scene = 0; scene < 40; ++scene) {
        const glm::vec3 player(coord(rng) * 0.2f, coord(rng) * 0.2f, 100.0f);
        // Low suns too: the box then reaches a long way toward the sun.
        const glm::vec3 sun(unit(rng), unit(rng), -0.15f - 0.85f * std::abs(unit(rng)));
        const float distance = scene % 2 ? 300.0f : 500.0f;
        const glm::mat4 light = sunMatrix(player, sun, distance);
        const float shadowRadius = distance * 1.35f;

        M2CullGrid grid;
        struct Inst { glm::vec3 pos; float r; };
        std::vector<Inst> all;
        for (uint32_t index = 0; index < 3000; ++index) {
            const Inst inst{{coord(rng), coord(rng), height(rng)}, radius(rng)};
            all.push_back(inst);
            place(grid, index, inst.pos, inst.pos, inst.r * 2.0f, inst.r * 1.5f);
        }

        size_t lost = 0;
        for (const auto& cell : grid.cells()) {
            const bool kept = cellMayCastShadow(light, cell.posMin, cell.posMax,
                                                cell.maxShadowRadius / shadowRadius);
            if (kept) continue;
            for (const uint32_t index : cell.members) {
                const Inst& inst = all[index];
                if (instanceCasts(light, inst.pos, shadowRadius, inst.r)) ++lost;
            }
        }
        CHECK(lost == 0);
    }
}

TEST_CASE("the light's columns are a small part of a large world") {
    // Not a correctness rule, but the point of the exercise: around a player
    // in the middle of 3 km of doodads, most columns are rejected.
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> coord(-1600.0f, 1600.0f);
    M2CullGrid grid;
    for (uint32_t index = 0; index < 20000; ++index) {
        const glm::vec3 pos(coord(rng), coord(rng), 50.0f);
        place(grid, index, pos, pos, 3.0f, 3.0f);
    }
    const glm::mat4 light = sunMatrix(glm::vec3(0, 0, 50), glm::vec3(-0.3f, -0.5f, -0.8f), 300.0f);
    size_t kept = 0;
    for (const auto& cell : grid.cells()) {
        if (cellMayCastShadow(light, cell.posMin, cell.posMax, cell.maxShadowRadius / 405.0f))
            kept += cell.members.size();
    }
    CHECK(kept < 20000 / 4);
}

TEST_CASE("a column is kept whenever one of its instances is in view") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> coord(-1500.0f, 1500.0f);
    std::uniform_real_distribution<float> radius(0.0f, 60.0f);
    std::uniform_real_distribution<float> yaw(0.0f, 6.2831853f);

    M2CullGrid grid;
    struct Inst { glm::vec3 pos, center; float r; };
    std::vector<Inst> all;
    for (uint32_t index = 0; index < 5000; ++index) {
        const glm::vec3 pos(coord(rng), coord(rng), 40.0f);
        // A cull sphere offset from the origin, as a tree's canopy is.
        const Inst inst{pos, pos + glm::vec3(radius(rng) * 0.3f, 0, radius(rng)), radius(rng)};
        all.push_back(inst);
        place(grid, index, inst.pos, inst.center, inst.r, 1.0f);
    }

    for (int view = 0; view < 30; ++view) {
        const glm::vec3 eye(coord(rng) * 0.3f, coord(rng) * 0.3f, 60.0f);
        const float a = yaw(rng);
        const glm::mat4 vp = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.5f, 3000.0f) *
                             glm::lookAt(eye, eye + glm::vec3(std::cos(a), std::sin(a), -0.1f),
                                         glm::vec3(0, 0, 1));
        Frustum frustum;
        frustum.extractFromMatrix(vp);
        const float maxDistSq = 600.0f * 600.0f;

        size_t lost = 0;
        for (const auto& cell : grid.cells()) {
            const bool kept = distanceSqToBox(eye, cell.viewMin, cell.viewMax) <= maxDistSq &&
                              frustum.intersectsAABB(cell.viewMin, cell.viewMax);
            if (kept) continue;
            for (const uint32_t index : cell.members) {
                const Inst& inst = all[index];
                const glm::vec3 d = inst.pos - eye;
                if (glm::dot(d, d) <= maxDistSq && frustum.intersectsSphere(inst.center, inst.r)) ++lost;
            }
        }
        CHECK(lost == 0);
    }
}

TEST_CASE("moving keeps an instance in the one column its position is in") {
    M2CullGrid grid;
    place(grid, 0, {10, 10, 0}, {10, 10, 0}, 1, 1);
    place(grid, 1, {20, 10, 0}, {20, 10, 0}, 1, 1);
    place(grid, 0, {500, 500, 0}, {500, 500, 0}, 1, 1);  // to another column
    place(grid, 1, {25, 12, 0}, {25, 12, 0}, 1, 1);      // within its column
    const auto* moved = columnOf(grid, 0);
    REQUIRE(moved != nullptr);
    CHECK(moved->posMin == glm::vec3(500, 500, 0));
    const auto* stayed = columnOf(grid, 1);
    REQUIRE(stayed != nullptr);
    CHECK(stayed->posMax == glm::vec3(25, 12, 0));
    CHECK(grid.filed() == 2);
}

TEST_CASE("a hidden instance is in no column until it is shown") {
    M2CullGrid grid;
    place(grid, 0, {10, 10, 0}, {10, 10, 0}, 1, 1);
    grid.record(0).flags |= M2CullGrid::kHidden;
    grid.file(0);
    CHECK(columnOf(grid, 0) == nullptr);
    grid.record(0).flags &= ~M2CullGrid::kHidden;
    grid.file(0);
    CHECK(columnOf(grid, 0) != nullptr);
}

TEST_CASE("a swap-remove leaves the last instance in its column under the index it moved to") {
    // As M2Renderer removes an instance: the last one takes its place.
    M2CullGrid grid;
    place(grid, 0, {10, 10, 0}, {10, 10, 0}, 1, 1);      // column A
    place(grid, 1, {200, 10, 0}, {200, 10, 0}, 1, 1);    // column B
    place(grid, 2, {400, 10, 0}, {400, 10, 0}, 1, 1);    // column C
    grid.record(2).id = 77;

    grid.removeSwapLast(0);

    REQUIRE(grid.size() == 2);
    CHECK(grid.record(0).id == 77);
    const auto* c = columnOf(grid, 0);
    REQUIRE(c != nullptr);
    CHECK(c->posMin == glm::vec3(400, 10, 0));
    CHECK(columnOf(grid, 1) != nullptr);
    CHECK(columnOf(grid, 2) == nullptr);  // no index past the end is listed
    CHECK(grid.filed() == 2);

    // And the one it moved to can move and go again like any other.
    place(grid, 0, {10, 300, 0}, {10, 300, 0}, 1, 1);
    CHECK(columnOf(grid, 0)->posMin == glm::vec3(10, 300, 0));
    grid.removeSwapLast(1);
    grid.removeSwapLast(0);
    CHECK(grid.size() == 0);
    CHECK(grid.filed() == 0);
}
