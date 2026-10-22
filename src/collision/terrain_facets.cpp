#include "collision/terrain_facets.hpp"
#include <bit>
#include <cassert>
#include <cmath>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define WOWEE_TERRAIN_SSE 1
#endif

namespace wowee::collision {
namespace {
// InitializeVertexGrid (0x7c3c60). Keep the inner-row subtraction in the
// original register precision, before storing the final grid coordinate.
const std::array<glm::vec2,145> grid = [] {
    std::array<glm::vec2,145> result;
    size_t index=0;
    for (int row=0;row<9;++row) {
        const double x=double(row)*-4.1666665f;
        for (int col=0;col<9;++col)
            result[index++]={float(x),float(0.0-double(col)*4.1666665f)};
        if (row<8) for (int col=0;col<8;++col)
            result[index++]={float(x-2.0833333f),float(0.0-double(col)*4.1666665f-2.0833333f)};
    }
    return result;
}();

uint8_t outcode(const glm::vec3& p,const glm::vec3& low,const glm::vec3& high) {
    uint8_t code=0;
    // ComputeAaBoxOutcode (0x7a61d0) tests the sign bits AFTER a float store,
    // with this authored margin. A generic strict AABB test is different.
    constexpr float margin=0.01944444328546524f;
    for (int axis=0;axis<3;++axis) {
        const float a=float(double(p[axis])-low[axis]+margin);
        const float b=float(double(high[axis])-p[axis]+margin);
        code |= uint8_t((std::bit_cast<uint32_t>(a)>>31)<<axis);
        code |= uint8_t((std::bit_cast<uint32_t>(b)>>31)<<(axis+3));
    }
    return code;
}

MovementPlane terrainPlane(const std::array<glm::vec3,3>& vertices,FacetNormalization mode) {
    const auto u=glm::dvec3(vertices[1])-glm::dvec3(vertices[0]);
    const auto v=glm::dvec3(vertices[2])-glm::dvec3(vertices[0]);
    glm::vec3 n(float(u.y*v.z-u.z*v.y),float(u.z*v.x-u.x*v.z),float(u.x*v.y-u.y*v.x));
    double inverse;
#ifdef WOWEE_TERRAIN_SSE
    if (mode==FacetNormalization::NativeSse) {
        // Unlike TriDataToFacetData, terrain uses scalar SSE for this sum.
        const float squared=(n.x*n.x+n.y*n.y)+n.z*n.z;
        inverse=_mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(squared)));
    } else
#endif
    {
        inverse=1.0/std::sqrt((double(n.x)*n.x+double(n.y)*n.y)+double(n.z)*n.z);
    }
    const glm::dvec3 scaled=glm::dvec3(n)*inverse;
    // Both paths retain the unrounded scaled normal for the plane offset,
    // but they add its products in different orders. No shared glm::dot.
    const auto& p=vertices[0];
    const double offset=mode==FacetNormalization::NativeSse
        ? -((scaled.z*p.z+scaled.y*p.y)+scaled.x*p.x)
        : -((scaled.x*p.x+scaled.y*p.y)+scaled.z*p.z);
    return {glm::vec3(scaled),float(offset)};
}
}

std::optional<std::vector<MovementFacet>> queryTerrainFacets(
    std::span<const float,145> heights,uint16_t holes,const glm::vec3& corner,
    TerrainFacetCells cells,const glm::vec3& queryMin,const glm::vec3& queryMax,
    FacetNormalization normalization) {
#ifndef WOWEE_TERRAIN_SSE
    if (normalization==FacetNormalization::NativeSse) return std::nullopt;
#endif
    assert(cells.minX>=0 && cells.minY>=0 && cells.maxX<=7 && cells.maxY<=7);
    std::vector<MovementFacet> result;
    constexpr std::array<int,5> subVertices{0,9,17,1,18};
    constexpr std::array<int,12> triangles{17,9,0,9,1,0,9,17,18,9,18,1};
    for (int y=cells.minY;y<=cells.maxY;++y) for (int x=cells.minX;x<=cells.maxX;++x) {
        if (holes & (1u<<((y/2)*4+x/2))) continue;
        const int base=y*17+x;
        std::array<glm::vec3,19> local;
        std::array<uint8_t,19> codes{};
        for (int i:subVertices) {
            local[i]={grid[base+i],heights[base+i]};
            codes[i]=outcode(local[i],queryMin,queryMax);
        }
        for (size_t i=0;i<triangles.size();i+=3) {
            const int a=triangles[i],b=triangles[i+1],c=triangles[i+2];
            if (codes[a]&codes[b]&codes[c]) continue;
            MovementFacet facet;
            facet.vertices={local[a]+corner,local[b]+corner,local[c]+corner};
            facet.plane=terrainPlane(facet.vertices,normalization);
            result.push_back(facet);
        }
    }
    return result;
}
std::vector<MovementFacet> queryImpassableTerrainFacets(
    const glm::vec3& low, const glm::vec3& high,
    const glm::vec3& queryMin, const glm::vec3& queryMax) {
    std::vector<MovementFacet> result;
    auto wall = [&](const glm::vec3& origin, const glm::vec3& edge, const glm::vec3& normal) {
        const glm::dvec3 up(0, 0, 32000);
        const auto start = glm::dvec3(origin);
        const auto end = start + glm::dvec3(edge);
        const MovementPlane plane{normal, float(-((double(normal.z) * origin.z +
            double(normal.x) * origin.x) + double(normal.y) * origin.y))};
        MovementFacet first;
        first.plane = plane;
        first.vertices = {origin, glm::vec3(end), glm::vec3(end + up)};
        result.push_back(first);
        first.vertices = {origin, glm::vec3(end + up), glm::vec3(start + up)};
        result.push_back(first);
    };
    if (low.y > queryMin.y)
        wall(low, {high.x - low.x, 0, 0}, {0, -1, 0});
    if (high.y < queryMax.y)
        wall({high.x, high.y, low.z}, {low.x - high.x, 0, 0}, {0, 1, 0});
    if (low.x > queryMin.x)
        wall({low.x, high.y, low.z}, {0, low.y - high.y, 0}, {-1, 0, 0});
    if (high.x < queryMax.x)
        wall({high.x, low.y, low.z}, {0, high.y - low.y, 0}, {1, 0, 0});
    return result;
}
std::vector<MovementFacet> queryFlightBoundsFacets(
    std::span<const int16_t,18> heights, const glm::vec3& corner,
    float queryMinZ, float queryMaxZ) {
    constexpr std::array<int,24> faces{3,0,4,0,1,4,1,2,4,2,5,4,5,8,4,8,7,4,7,6,4,6,3,4};
    constexpr std::array<float,3> offsets{0.0f,-266.6666564941406f,-533.3333129882812f};
    const auto cut=[](double z) {
        assert(z >= -2147483648.0 && z < 2147483648.0);
        return std::bit_cast<int16_t>(uint16_t(int32_t(z)));
    };
    const int16_t top=cut(double(queryMaxZ)+1), bottom=cut(double(queryMinZ)-1);
    std::vector<MovementFacet> result;
    for (int side=0;side<2;++side) {
        for (size_t tri=0;tri<faces.size();tri+=3) {
            auto outside=[&](int i) { return side ? bottom>heights[9+i] : top<heights[i]; };
            if (outside(faces[tri]) && outside(faces[tri+1]) && outside(faces[tri+2])) continue;
            MovementFacet facet;
            for (int j=0;j<3;++j) {
                const int i=faces[tri+j];
                facet.vertices[side && j ? 3-j : j]={
                    float(double(offsets[i/3])+corner.x),float(double(offsets[i%3])+corner.y),
                    float(heights[side*9+i])};
            }
            facet.plane=terrainPlane(facet.vertices,FacetNormalization::X87);
            result.push_back(facet);
        }
    }
    return result;
}
} // namespace wowee::collision
