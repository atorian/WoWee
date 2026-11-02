// Where each resident terrain chunk is, by its place in the renderer's list:
// found by its place on the map, linked to its neighbours there, and marked
// in the coverage the distant terrain leaves to it - kept right as chunks
// come and go, the leaving ones filled by the last of the list.
#include <catch_amalgamated.hpp>

#include "rendering/terrain_chunk_index.hpp"

#include <map>
#include <random>
#include <utility>
#include <vector>

using wowee::rendering::TerrainChunkIndex;

namespace {

/// The chunks, by list place, as the renderer's list holds them.
using Places = std::vector<std::pair<int, int>>;

/// What the index must say, worked out from the list alone.
void requireAgrees(const TerrainChunkIndex& index, const Places& places) {
    REQUIRE(index.size() == places.size());
    std::map<std::pair<int, int>, uint32_t> at;
    for (uint32_t i = 0; i < places.size(); ++i) at[places[i]] = i;
    const auto placeOf = [&](int gx, int gy) {
        const auto it = at.find({gx, gy});
        return it == at.end() ? TerrainChunkIndex::kNone : it->second;
    };
    for (uint32_t i = 0; i < places.size(); ++i) {
        const auto [gx, gy] = places[i];
        REQUIRE(index.find(gx, gy) == i);
        // y-1, y+1, x-1, x+1, as a chunk's skirts are ordered.
        REQUIRE(index.neighbour(i, 0) == placeOf(gx, gy - 1));
        REQUIRE(index.neighbour(i, 1) == placeOf(gx, gy + 1));
        REQUIRE(index.neighbour(i, 2) == placeOf(gx - 1, gy));
        REQUIRE(index.neighbour(i, 3) == placeOf(gx + 1, gy));
    }
    // The coverage: a chunk's cell is set exactly where a chunk is.
    size_t set = 0;
    for (const auto& tile : index.coverage()) {
        for (const uint32_t word : tile) set += static_cast<size_t>(__builtin_popcount(word));
    }
    REQUIRE(set == places.size());
    for (const auto& [gx, gy] : places) {
        const int cell = (gy % 16) * 16 + gx % 16;
        REQUIRE((index.coverage()[(gy / 16) * 64 + gx / 16][cell / 32] >> (cell % 32) & 1u) == 1u);
    }
}

/// Removes the list places in `leaving` as TerrainRenderer::removeTile does:
/// highest first, each filled by the last.
void removeAsTheRendererDoes(TerrainChunkIndex& index, Places& places, std::vector<uint32_t> leaving) {
    std::sort(leaving.begin(), leaving.end(), std::greater<>());
    for (const uint32_t i : leaving) {
        index.removeSwapLast(i);
        places[i] = places.back();
        places.pop_back();
    }
}

}  // namespace

TEST_CASE("a chunk knows the chunks beside it, and they know it") {
    TerrainChunkIndex index;
    // A row of three along x, and one above the middle.
    const uint32_t west = index.add(32 * 16 + 4, 30 * 16 + 7);
    const uint32_t middle = index.add(32 * 16 + 5, 30 * 16 + 7);
    const uint32_t east = index.add(32 * 16 + 6, 30 * 16 + 7);
    const uint32_t above = index.add(32 * 16 + 5, 30 * 16 + 8);
    CHECK(index.neighbour(middle, 2) == west);
    CHECK(index.neighbour(middle, 3) == east);
    CHECK(index.neighbour(middle, 1) == above);
    CHECK(index.neighbour(middle, 0) == TerrainChunkIndex::kNone);
    CHECK(index.neighbour(west, 3) == middle);
    CHECK(index.neighbour(above, 0) == middle);

    // The middle one leaves; the last of the list, the one above, fills its
    // place and keeps its own neighbours there.
    index.removeSwapLast(middle);
    CHECK(index.find(32 * 16 + 5, 30 * 16 + 7) == TerrainChunkIndex::kNone);
    CHECK(index.find(32 * 16 + 5, 30 * 16 + 8) == middle);
    CHECK(index.neighbour(west, 3) == TerrainChunkIndex::kNone);
    CHECK(index.neighbour(middle, 0) == TerrainChunkIndex::kNone);
}

TEST_CASE("the index stays right through tiles streamed in and out") {
    std::mt19937 rng(20261102);
    TerrainChunkIndex index;
    Places places;
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> tiles;
    // Tiles in a 6x6 patch, each with a random part of its 256 chunks, as a
    // tile still uploading has; added and removed in random order.
    std::uniform_int_distribution<int> tileAxis(30, 35), keepChance(0, 3);
    for (int step = 0; step < 300; ++step) {
        const std::pair<int, int> tile{tileAxis(rng), tileAxis(rng)};
        if (const auto it = tiles.find(tile); it == tiles.end()) {
            auto& added = tiles[tile];
            for (int cy = 0; cy < 16; ++cy) {
                for (int cx = 0; cx < 16; ++cx) {
                    if (keepChance(rng) == 0) continue;
                    const int gx = tile.first * 16 + cx, gy = tile.second * 16 + cy;
                    REQUIRE(index.add(gx, gy) == places.size());
                    places.emplace_back(gx, gy);
                    added.emplace_back(gx, gy);
                }
            }
        } else {
            std::vector<uint32_t> leaving;
            for (const auto& place : it->second) leaving.push_back(index.find(place.first, place.second));
            removeAsTheRendererDoes(index, places, leaving);
            tiles.erase(it);
        }
        if (step % 25 == 0) requireAgrees(index, places);
    }
    requireAgrees(index, places);

    index.clear();
    places.clear();
    requireAgrees(index, places);
}
