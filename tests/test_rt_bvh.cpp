#include "rendering/rt_region.hpp"
// The hierarchy the software ray tracer walks.
//
// The structural checks are what the shader relies on without re-checking:
// every triangle reachable exactly once, and every box enclosing what is under
// it. The walk itself runs on the GPU (rt_trace_sw.glsli).
#include <catch_amalgamated.hpp>

#include <cmath>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/rt_bvh.hpp"
#include "rendering/rt_range_allocator.hpp"

using namespace wowee::rendering;

namespace {

std::vector<RtTriangle> randomSoup(uint32_t count, uint32_t seed, float spread, float size) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> pos(-spread, spread);
    std::uniform_real_distribution<float> off(-size, size);
    std::vector<RtTriangle> tris(count);
    for (auto& t : tris) {
        const glm::vec3 c(pos(rng), pos(rng), pos(rng));
        t.v0 = glm::vec4(c + glm::vec3(off(rng), off(rng), off(rng)), 0.0f);
        t.v1 = glm::vec4(c + glm::vec3(off(rng), off(rng), off(rng)), 0.0f);
        t.v2 = glm::vec4(c + glm::vec3(off(rng), off(rng), off(rng)), 0.0f);
    }
    return tris;
}

bool contains(const RtBvhNode& outer, const glm::vec3& p) {
    constexpr float eps = 1e-4f;
    return glm::all(glm::greaterThanEqual(p, outer.bmin - eps)) &&
           glm::all(glm::lessThanEqual(p, outer.bmax + eps));
}

void checkStructure(const RtBvh& bvh, const std::vector<RtTriangle>& tris) {
    std::vector<int> seen(tris.size(), 0);
    std::vector<uint32_t> stack{0};
    while (!stack.empty()) {
        const uint32_t idx = stack.back();
        stack.pop_back();
        const RtBvhNode& n = bvh.nodes[idx];
        if (n.count > 0) {
            for (uint32_t i = n.leftOrFirst; i < n.leftOrFirst + n.count; ++i) {
                REQUIRE(i < tris.size());
                ++seen[i];
                CHECK(contains(n, glm::vec3(tris[i].v0)));
                CHECK(contains(n, glm::vec3(tris[i].v1)));
                CHECK(contains(n, glm::vec3(tris[i].v2)));
            }
        } else {
            REQUIRE(n.leftOrFirst + 1 < bvh.nodes.size());
            for (uint32_t c : {n.leftOrFirst, n.leftOrFirst + 1}) {
                CHECK(contains(n, bvh.nodes[c].bmin));
                CHECK(contains(n, bvh.nodes[c].bmax));
                stack.push_back(c);
            }
        }
    }
    for (int s : seen) CHECK(s == 1);
}

}  // namespace

TEST_CASE("RT BVH reaches every triangle of a soup once, inside its boxes", "[rt_bvh]") {
    for (uint32_t seed : {1u, 2u, 3u}) {
        auto tris = randomSoup(2000, seed, 50.0f, 3.0f);
        const RtBvh bvh = buildRtTriangleBvh(tris);
        checkStructure(bvh, tris);
    }
}

TEST_CASE("RT BVH splits coincident triangles instead of one huge leaf", "[rt_bvh]") {
    // Stacked copies of one triangle - the case where no SAH split separates
    // anything. Decals and double-sided cards in the models look like this.
    std::vector<RtTriangle> tris(200);
    for (auto& t : tris) {
        t.v0 = {0, 0, 0, 0};
        t.v1 = {1, 0, 0, 0};
        t.v2 = {0, 1, 0, 0};
    }
    const RtBvh bvh = buildRtTriangleBvh(tris, 4);
    checkStructure(bvh, tris);
    for (const auto& n : bvh.nodes) CHECK(n.count <= 16);
}

TEST_CASE("RT BVH of nothing has no nodes", "[rt_bvh]") {
    std::vector<RtTriangle> tris;
    const RtBvh bvh = buildRtTriangleBvh(tris);
    CHECK(bvh.nodes.empty());
}

TEST_CASE("RT surface packing round-trips to 8 bits", "[rt_bvh]") {
    const glm::vec4 v = unpackRtSurface(packRtSurface({0.25f, 0.5f, 1.0f}, 0.3f));
    CHECK(v.r == Catch::Approx(0.25f).margin(1.0 / 255));
    CHECK(v.g == Catch::Approx(0.5f).margin(1.0 / 255));
    CHECK(v.b == Catch::Approx(1.0f).margin(1.0 / 255));
    CHECK(v.a == Catch::Approx(0.3f).margin(1.0 / 255));
}


TEST_CASE("RT range allocator reuses and coalesces freed ranges", "[rt_bvh]") {
    RtRangeAllocator a(100);
    const uint32_t x = a.allocate(40);
    const uint32_t y = a.allocate(40);
    CHECK(x == 0);
    CHECK(y == 40);
    CHECK(a.allocate(30) == RtRangeAllocator::kFailed);
    CHECK(a.used() == 80);

    a.release(x, 40);
    a.release(y, 40);
    CHECK(a.used() == 0);
    CHECK(a.allocate(100) == 0);  // only possible once all three ranges merged
    a.release(0, 100);

    const uint32_t z = a.allocate(90);
    a.grow(200);
    CHECK(a.capacity() == 200);
    CHECK(a.used() == 90);
    CHECK(a.allocate(110) == 90);  // old tail and new space are one range
    a.release(z, 90);
    CHECK(a.allocate(0) == RtRangeAllocator::kFailed);
}

TEST_CASE("RT region keeps offscreen sunward casters and intersecting bounds", "[rt_region]") {
    RtRegion region;
    region.distance = 100;
    region.indirectLength = 0;
    region.sunDirection = {1, 0, 0};
    CHECK(region.keeps({{-5, -5, -5}, {5, 5, 5}}, false));
    CHECK(region.keeps({{900, -2, -2}, {910, 2, 2}}, false));
    CHECK_FALSE(region.keeps({{-910, -2, -2}, {-900, 2, 2}}, false));
    CHECK_FALSE(region.keeps({{900, 500, -2}, {910, 510, 2}}, false));
    // The centre is outside the receiver area; part of a large building is in it.
    region.sunlight = false;
    CHECK(region.keeps({{90, -20, -20}, {1000, 20, 20}}, false));
    CHECK_FALSE(region.keeps({{200, -20, -20}, {1000, 20, 20}}, false));
}

TEST_CASE("RT residency has a margin and includes indirect shadow origins", "[rt_region]") {
    RtRegion region;
    region.distance = 100;
    region.indirectLength = 0;
    region.sunlight = false;
    RtAabb edge{{108, -1, -1}, {110, 1, 1}};
    CHECK_FALSE(region.keeps(edge, false));
    CHECK(region.keeps(edge, true));
    region.indirectLength = 120;
    CHECK(region.keeps({{200, -1, -1}, {210, 1, 1}}, false));
    region.centre = {1000, 0, 0};
    CHECK_FALSE(region.keeps(edge, true));
}
