#include "collision/liquid_facets.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>

namespace wowee::collision {
std::vector<glm::vec3> makeTerrainLiquidVertices(
    int chunkX, int chunkY, TerrainFacetCells bounds, float baseZ,
    std::span<const float> heights, uint16_t vertexFormat) {
    assert(chunkX>=0 && chunkX<1024 && chunkY>=0 && chunkY<1024);
    assert(bounds.minX>=0 && bounds.minY>=0 && bounds.maxX<=8 && bounds.maxY<=8);
    assert(bounds.minX<bounds.maxX && bounds.minY<bounds.maxY);
    assert(heights.size()==size_t((bounds.maxX-bounds.minX+1)*(bounds.maxY-bounds.minY+1)));
    auto step=[](int index) {
        const double a=17066.666f-double(index)*33.333332f;
        const double b=17066.666f-double(index+1)*33.333332f;
        return float(-(a-b)*0.125f);
    };
    const float stepX=step(chunkY),stepY=step(chunkX);
    std::vector<glm::vec3> vertices;
    vertices.reserve(heights.size());
    for (int row=bounds.minY;row<=bounds.maxY;++row) {
        const float x=float(double(row)*stepX);
        for (int col=bounds.minX;col<=bounds.maxX;++col) {
            const float height=vertexFormat>1 ? 0.0f : heights[vertices.size()];
            vertices.emplace_back(x,float(double(col)*stepY),float(double(height)-baseZ));
        }
    }
    return vertices;
}

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
std::vector<glm::vec3> makeWmoLiquidVertices(
    uint32_t xVerts,uint32_t yVerts,const glm::vec3& corner,std::span<const float> heights) {
    assert(heights.size()==size_t(xVerts)*yVerts);
    std::vector<glm::vec3> vertices;
    vertices.reserve(heights.size());
    double y=corner.y;
    for (uint32_t row=0;row<yVerts;++row,y+=4.1666665f) {
        double x=corner.x;
        for (uint32_t col=0;col<xVerts;++col,x+=4.1666665f)
            vertices.emplace_back(float(x),float(y),heights[vertices.size()]);
    }
    return vertices;
}
std::optional<LiquidFacetQuery> queryWmoLiquidFacets(
    const WmoLiquidGrid& grid,const glm::vec3& queryMin,const glm::vec3& queryMax,
    uint32_t flags,const glm::mat4& transform,uint64_t owner,FacetNormalization normalization) {
    LiquidFacetQuery result;
    if (!grid.liquidType || !grid.xTiles || !grid.yTiles) return result;
    if ((flags&0x10000) && !(flags&0x20000) && !(grid.liquidFlags&4)) return result;
    assert(grid.xVerts==grid.xTiles+1 && grid.yVerts==grid.yTiles+1);
    assert(grid.vertices.size()==size_t(grid.xVerts)*grid.yVerts && grid.vertices.size()<=65536);
    assert(grid.tiles.size()==size_t(grid.xTiles)*grid.yTiles);
    const glm::vec2 a=glm::vec2(queryMin-grid.corner),b=glm::vec2(queryMax-grid.corner);
    const auto low=glm::min(a,b),high=glm::max(a,b);
    // WMO uses 0x3e75c290, one float above the terrain map's 0.24f.
    // Only maxX retains the multiplication in x87 register precision before
    // floor; the other three products are stored as float by the original.
    int maxX=int(std::floor(double(high.x)*0.24000000953674316f));
    int maxY=int(std::floor(float(double(high.y)*0.24000000953674316f)));
    int minX=int(std::floor(float(double(low.x)*0.24000000953674316f)));
    int minY=int(std::floor(float(double(low.y)*0.24000000953674316f)));
    if (maxX<0 || maxY<0 || minX>=int(grid.xTiles) || minY>=int(grid.yTiles)) return result;
    minX=std::max(0,minX);minY=std::max(0,minY);
    maxX=std::min(int(grid.xTiles)-1,maxX);maxY=std::min(int(grid.yTiles)-1,maxY);
    if (size_t(maxX-minX+2)*size_t(maxY-minY+2)>8192) return result;
    const auto code=[&](const glm::vec3& v) {
        const float below=float(double(v.z)-queryMin.z+0.019444443f);
        const float above=float(double(queryMax.z)-v.z+0.019444443f);
        return ((std::bit_cast<uint32_t>(below)>>29)&4) | ((std::bit_cast<uint32_t>(above)>>26)&32);
    };
    for (int row=minY;row<=maxY;++row) for (int col=minX;col<=maxX;++col) {
        if ((grid.tiles[row*grid.xTiles+col]&15)==15) continue;
        const size_t base=row*grid.xVerts+col;
        const std::array<size_t,6> indices{base,base+grid.xVerts+1,base+grid.xVerts,base,base+1,base+grid.xVerts+1};
        for (int face=0;face<2;++face) {
            const std::array<glm::vec3,3> v{grid.vertices[indices[face*3]],grid.vertices[indices[face*3+1]],grid.vertices[indices[face*3+2]]};
            if (code(v[0])&code(v[1])&code(v[2])) continue;
            auto built=makeMovementFacet(v,transform,owner,normalization);
            if (!built) return std::nullopt;
            result.degenerateFaces+=uint32_t(built->degenerate);
            result.facets.push_back(built->facet);
        }
    }
    if (!result.facets.empty()) result.indexReservation=6u*(maxX-minX+1)*(maxY-minY+1);
    return result;
}
} // namespace wowee::collision
