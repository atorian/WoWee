#include "collision/world_facets.hpp"
#include <iterator>

namespace wowee::collision {
WorldMovementFacets::WorldMovementFacets(const WorldFacetView& world,uint32_t mapId,bool dungeon,
    const TerrainLiquidMaterials& terrainMaterials,const WmoLiquidMaterials& wmoMaterials,
    EntityFacetProvider* entities,FacetNormalization normalization,uint32_t initialQueryTag)
    :MapMovementFacetSource(dungeon),world_(world),mapId_(mapId),queryTag_(initialQueryTag),
     terrainMaterials_(terrainMaterials),wmoMaterials_(wmoMaterials),entities_(entities),normalization_(normalization) {}
MovementGeometryStatus WorldMovementFacets::queryMapObjects(
    const MovementFacetRequest& request,uint32_t flags,std::vector<MovementFacet>& output) {
    ++queryTag_;
    const auto result=appendWorldWmoFacets(world_.buildings,world_.proxies,request,flags,
        wmoMaterials_,*this,normalization_,output);
    statusFlags_=result.originalStatusFlags;
    degenerateFaces_=result.degenerateFaces;
    return result.status;
}
void WorldMovementFacets::appendLinks(const WorldFacetLinks& links,
    const MovementFacetRequest& request,uint32_t flags,std::vector<MovementFacet>& output) {
    appendDoodadFacets(links.doodads,request,flags,queryTag_,output);
    appendEntityFacets(links.entities,entities_,request,flags,queryTag_,output);
}
MovementGeometryStatus WorldMovementFacets::queryChunk(int x,int y,TerrainFacetCells cells,
    const MovementFacetRequest& request,uint32_t flags,std::vector<MovementFacet>& output) {
    const auto& area=world_.areas[((x>>4)&63)+(((y>>4)&63)*64)];
    if (area.state==MovementGeometryStatus::Failed) return area.state;
    if (area.state!=MovementGeometryStatus::Ready)
        return appendUnavailableTerrainFacets(x,y,area.hasTerrain,request,flags,output);
    if (!area.terrain || !area.terrain->collisionDataValid) return MovementGeometryStatus::Failed;
    const auto index=size_t((y&15)*16+(x&15));
    const auto result=appendTerrainChunkFacets(area.terrain->chunks[index],x,y,mapId_,cells,
        request,flags,terrainMaterials_,normalization_,output);
    if (result!=MovementGeometryStatus::Ready) return result;
    if (index>=area.chunks.size()) return MovementGeometryStatus::Failed;
    appendLinks(area.chunks[index],request,flags,output);
    return MovementGeometryStatus::Ready;
}
MovementGeometryStatus WorldMovementFacets::queryFlightBounds(int x,int y,
    const MovementFacetRequest& request,std::vector<MovementFacet>& output) {
    if (x<0 || y<0 || x>=64 || y>=64) return MovementGeometryStatus::Failed;
    const auto& area=world_.areas[size_t(y*64+x)];
    if (area.state!=MovementGeometryStatus::Ready) return area.state;
    if (!area.terrain || !area.terrain->collisionDataValid) return MovementGeometryStatus::Failed;
    if (area.terrain->flightBounds) {
        auto facets=queryFlightBoundsFacets(*area.terrain->flightBounds,area.flightCorner,
            request.queryMin.z,request.queryMax.z);
        output.insert(output.end(),std::make_move_iterator(facets.begin()),std::make_move_iterator(facets.end()));
    }
    return MovementGeometryStatus::Ready;
}
MovementGeometryStatus WorldMovementFacets::doodads(uint64_t id,
    const MovementFacetRequest& request,uint32_t flags,std::vector<MovementFacet>& output) {
    const auto found=world_.buildingGroups.find(id);
    if (found==world_.buildingGroups.end()) return MovementGeometryStatus::Failed;
    appendDoodadFacets(found->second.doodads,request,flags,queryTag_,output);
    return MovementGeometryStatus::Ready;
}
MovementGeometryStatus WorldMovementFacets::destructibles(uint64_t id,
    const MovementFacetRequest& request,uint32_t flags,std::vector<MovementFacet>& output) {
    const auto found=world_.buildingGroups.find(id);
    if (found==world_.buildingGroups.end()) return MovementGeometryStatus::Failed;
    appendEntityFacets(found->second.entities,entities_,request,flags,queryTag_,output);
    return MovementGeometryStatus::Ready;
}
} // namespace wowee::collision
