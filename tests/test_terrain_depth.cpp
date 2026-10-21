#include <catch_amalgamated.hpp>
#include "rendering/terrain_depth.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <numeric>

using namespace wowee::rendering;
namespace {
using Chunk = TerrainDepthChunk<int>;
using Cache = TerrainDepthCache<int>;
using Tile = TerrainDepthTile<int>;

std::shared_ptr<const Tile> snapshot(Cache& cache, const std::vector<Chunk>& source, size_t& reads) {
    std::vector<uint32_t> ids(source.size());
    std::iota(ids.begin(), ids.end(), 0);
    return cache.extract(ids, [&](uint32_t id) { ++reads; return source.at(id); });
}

glm::mat4 overhead() {
    return glm::orthoRH_ZO(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f) *
           glm::lookAt(glm::vec3(0, 0, 50), glm::vec3(0), glm::vec3(0, 1, 0));
}
}

TEST_CASE("unchanged terrain geometry is not read or copied into every frame", "[terrain-depth]") {
    Cache cache;
    std::vector<Chunk> source{{17, {0, 0, 0}, 1}, {23, {2, 0, 0}, 1}};
    size_t reads = 0;
    auto first = snapshot(cache, source, reads);
    reads = 0;
    for (int frame = 0; frame < 20; ++frame) {
        auto current = snapshot(cache, source, reads);
        std::vector<int> drawn;
        visitTerrainDepth<int>({current}, overhead(), {}, 5, [&](int id) { drawn.push_back(id); });
        CHECK(drawn == std::vector<int>{17, 23});
    }
    CHECK(reads == 0);
}

TEST_CASE("streaming updates the next terrain snapshot while a retained frame remains drawable", "[terrain-depth]") {
    std::shared_ptr<const Tile> old;
    size_t reads = 0;
    {
        Cache cache;
        std::vector<Chunk> source{{1, {0, 0, 0}, 1}};
        old = snapshot(cache, source, reads);
        source.push_back({2, {2, 0, 0}, 1});
        cache.invalidate();
        auto grown = snapshot(cache, source, reads);
        std::vector<int> drawn;
        visitTerrainDepth<int>({grown}, overhead(), {}, 5, [&](int id) { drawn.push_back(id); });
        CHECK(drawn == std::vector<int>{1, 2});
        source.clear();
        cache.invalidate();
        auto empty = snapshot(cache, source, reads);
        CHECK(visitTerrainDepth<int>({empty}, overhead(), {}, 5, [](int) {}).drawn == 0);
    }
    std::vector<int> drawn;
    visitTerrainDepth<int>({old}, overhead(), {}, 5, [&](int id) { drawn.push_back(id); });
    CHECK(drawn == std::vector<int>{1});
}

TEST_CASE("depth traversal rejects distant tiles before visiting their chunks", "[terrain-depth]") {
    Cache near, far;
    size_t reads = 0;
    std::vector<Chunk> distant;
    for (int i = 0; i < 256; ++i) distant.push_back({100 + i, {1000 + i % 16, 1000 + i / 16, 0}, 1});
    auto a = snapshot(near, {{1, {0, 0, 0}, 1}, {2, {4, 0, 0}, 1}}, reads);
    auto b = snapshot(far, distant, reads);
    std::vector<int> drawn;
    const auto visited = visitTerrainDepth<int>({a, b}, overhead(), {}, 5, [&](int id) { drawn.push_back(id); });
    CHECK(drawn == std::vector<int>{1, 2});
    CHECK(visited.chunksTested == 2);
}

TEST_CASE("tile bounds retain edge casters and apply depth reach only to orthographic maps", "[terrain-depth]") {
    for (const glm::vec3 origin : {glm::vec3(0), glm::vec3(6000, -500, 650)}) {
        CAPTURE(origin.x);
        Cache edge, front, back;
        size_t reads = 0;
        // The tile's centre lies outside the map; the first chunk touches it.
        auto a = snapshot(edge, {{1, origin + glm::vec3(10.5f, 0, 0), 1},
                                 {2, origin + glm::vec3(16, 0, 0), 1}}, reads);
        auto b = snapshot(front, {{3, origin + glm::vec3(0, 0, 40), 1}}, reads);
        auto c = snapshot(back, {{4, origin + glm::vec3(0, 0, -40), 1}}, reads);
        const auto shift = glm::translate(glm::mat4(1), -origin);
        std::vector<int> drawn;
        visitTerrainDepth<int>({a, b, c}, overhead() * shift, origin, 5, [&](int id) { drawn.push_back(id); });
        CHECK(drawn == std::vector<int>{1, 3});
        drawn.clear();
        const auto perspective = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 1.0f, 100.0f) *
            glm::lookAt(glm::vec3(0, 0, 50), glm::vec3(0), glm::vec3(0, 1, 0)) * shift;
        visitTerrainDepth<int>({a, b, c}, perspective, origin, 0, [&](int id) { drawn.push_back(id); });
        CHECK(drawn == std::vector<int>{1, 2, 3, 4});
    }
}
