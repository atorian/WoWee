#include "collision/liquid_facets.hpp"
#include <algorithm>
#include <bit>
#include <cassert>

namespace wowee::collision {
std::optional<LiquidFacetQuery> queryTerrainLiquidFacets(
    const TerrainLiquidGrid& grid, TerrainFacetCells cells,
    const glm::vec3& corner, float queryMinZ, float queryMaxZ,
    FacetNormalization normalization) {
    const auto bounds = grid.bounds;
    const int width = bounds.maxX - bounds.minX;
    assert(bounds.minX >= 0 && bounds.minY >= 0 && bounds.maxX <= 8 && bounds.maxY <= 8);
    assert(width > 0 && bounds.maxY > bounds.minY &&
        grid.vertices.size() == size_t((width+1)*(bounds.maxY-bounds.minY+1)));
    assert(grid.legacyTiles.empty() || grid.legacyTiles.size() == 64);
    const int minX = std::max(cells.minX, bounds.minX);
    const int minY = std::max(cells.minY, bounds.minY);
    const int maxX = std::min(cells.maxX+1, bounds.maxX);
    const int maxY = std::min(cells.maxY+1, bounds.maxY);
    const int stride = width+1;
    const std::array<int,6> triangles{0,stride,stride+1,0,stride+1,1};
    auto code = [&](float z) {
        const float below = float(double(z) - queryMinZ + 0.019444443f);
        const float above = float(double(queryMaxZ) - z + 0.019444443f);
        return ((std::bit_cast<uint32_t>(below)>>29)&4) | ((std::bit_cast<uint32_t>(above)>>26)&32);
    };
    glm::mat4 transform(1);
    transform[3] = glm::vec4(corner,1);
    LiquidFacetQuery result;
    for (int y=minY; y<maxY; ++y) for (int x=minX; x<maxX; ++x) {
        if (grid.legacyTiles.empty()) {
            if (!(grid.cellMask & (uint64_t{1} << ((y-bounds.minY)*width+x-bounds.minX)))) continue;
        } else {
            const uint32_t type = grid.legacyTiles[y*8+x]&15;
            if (type == 15 || ((type^(grid.liquidType-1))&3)) continue;
        }
        const int base = (y-bounds.minY)*stride+x-bounds.minX;
        for (int t=0; t<6; t+=3) {
            const std::array<glm::vec3,3> vertices{grid.vertices[base+triangles[t]],
                grid.vertices[base+triangles[t+1]],grid.vertices[base+triangles[t+2]]};
            if (code(vertices[0].z)&code(vertices[1].z)&code(vertices[2].z)) continue;
            auto built = makeMovementFacet(vertices,transform,0,normalization);
            if (!built) return std::nullopt;
            result.degenerateFaces += built->degenerate;
            result.facets.push_back(built->facet);
        }
    }
    return result;
}
} // namespace wowee::collision
