#include <catch_amalgamated.hpp>

#include <cmath>
#include <utility>

#include "pipeline/coordinates.hpp"
#include "pipeline/terrain_mesh.hpp"

// Which render axis a tile index counts along.
//
// worldToTile takes the tile's X index from renderY and its Y index from
// renderX - the grid is turned a quarter turn from the axes it is addressed
// in. So a tile's render-X edge comes from its *Y* index, and anything that
// works out the tile's extent has to cross the axes the same way.
//
// TerrainManager::getTileBounds did not: it named its outputs for one axis and
// computed them from the other, which put every chunk-index guess a tile's
// worth of chunks away from the point being asked about. Nothing showed,
// because each caller checked its guess against the chunk's own position and
// widened the search - until the area lookup asked without checking, and
// answered with a chunk from far enough away to belong to another zone.
TEST_CASE("a tile's render-X edge comes from its Y index", "[coordinates]") {
    using wowee::core::coords::TILE_SIZE;
    using wowee::core::coords::worldToTile;

    // One tile west of the origin tile, so the two indices differ - the fault
    // is invisible on the diagonal, where x and y are the same number.
    constexpr int kTileX = 32, kTileY = 33;
    const float maxRenderX = (32 - kTileY) * TILE_SIZE;
    const float maxRenderY = (32 - kTileX) * TILE_SIZE;
    constexpr float kInside = 0.01f;

    // Both corners of that extent land in the tile it was worked out for.
    CHECK(worldToTile(maxRenderX - kInside, maxRenderY - kInside) ==
          std::pair<int, int>{kTileX, kTileY});
    CHECK(worldToTile(maxRenderX - TILE_SIZE + kInside,
                      maxRenderY - TILE_SIZE + kInside) ==
          std::pair<int, int>{kTileX, kTileY});

    // Crossing them the other way lands in a different tile, which is what
    // taking the X edge from the X index amounts to.
    CHECK(worldToTile(maxRenderY - kInside, maxRenderX - kInside) !=
          std::pair<int, int>{kTileX, kTileY});
}
