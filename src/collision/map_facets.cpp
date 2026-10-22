#include "collision/map_facets.hpp"
#include <cmath>

namespace wowee::collision {
namespace {
int cell(double distance) {
    const float scaled=float(distance*0.24f);
    // FISTP with the client's nearest/even control word. Do not use truncation
    // or floor at exact cell seams, and retain the intermediate float store.
    const double value=double(scaled)-0.5f;
    const double lower=std::floor(value),fraction=value-lower;
    const int result=int(lower);
    return result + int(fraction>0.5 || (fraction==0.5 && (result&1)));
}
MovementGeometryStatus combine(MovementGeometryStatus a,MovementGeometryStatus b) {
    if (a==MovementGeometryStatus::Failed || b==MovementGeometryStatus::Failed)
        return MovementGeometryStatus::Failed;
    if (a==MovementGeometryStatus::NotReady || b==MovementGeometryStatus::NotReady)
        return MovementGeometryStatus::NotReady;
    return MovementGeometryStatus::Ready;
}
}
MovementGeometryStatus appendUnavailableTerrainFacets(
    int x, int y, bool hasTerrain, const MovementFacetRequest& request,
    uint32_t flags, std::vector<MovementFacet>& output) {
    if (!hasTerrain) return MovementGeometryStatus::Ready;
    if (!(flags&0x80000000u)) return MovementGeometryStatus::NotReady;
    const auto edge=[](int index) { return float(double(index)*-33.333332f+17066.666f); };
    const glm::vec3 low(edge(y+1),edge(x+1),float(double(request.queryMin.z)-1000.0));
    const glm::vec3 high(edge(y),edge(x),float(double(request.queryMax.z)+1000.0));
    const bool intersects=high.x>=request.bodyMin.x && high.y>=request.bodyMin.y && high.z>=request.bodyMin.z &&
        low.x<=request.bodyMax.x && low.y<=request.bodyMax.y && low.z<=request.bodyMax.z;
    if (intersects) return MovementGeometryStatus::NotReady;
    appendMovementBoxFacets(low,high,output);
    return MovementGeometryStatus::Ready;
}
MovementGeometryStatus MapMovementFacetSource::query(
    const MovementFacetRequest& request,uint32_t flags,std::vector<MovementFacet>& output) {
    output.clear();
    auto status=queryMapObjects(request,flags,output);
    if (status!=MovementGeometryStatus::Ready || dungeon_) return status;
    const double topX=-(double(request.queryMax.x)-17066.666f);
    const double topY=-(double(request.queryMax.y)-17066.666f);
    const double bottomX=-(double(request.queryMin.x)-17066.666f);
    const double bottomY=-(double(request.queryMin.y)-17066.666f);
    if (topY<0 || topX<0 || bottomY>=34133.332f || bottomX>=34133.332f)
        return MovementGeometryStatus::NotReady;
    const TerrainFacetCells rect{cell(float(topY)),cell(float(topX)),cell(bottomY),cell(float(bottomX))};
    for (int y=rect.minY>>3;y<=rect.maxY>>3;++y)
        for (int x=rect.minX>>3;x<=rect.maxX>>3;++x)
            status=combine(status,queryChunk(x,y,rect,request,flags,output));
    if (flags&0x200) {
        for (int y=rect.minY>>7;y<=rect.maxY>>7;++y)
            for (int x=rect.minX>>7;x<=rect.maxX>>7;++x)
                status=combine(status,queryFlightBounds(x,y,request,output));
    }
    return status;
}
} // namespace wowee::collision
