#include "collision/terrain_chunk_facets.hpp"
#include "collision/liquid_facets.hpp"
#include <algorithm>
#include <iterator>

namespace wowee::collision {
MovementGeometryStatus appendTerrainChunkFacets(
    const TerrainTileHeights::Chunk& chunk,int globalX,int globalY,uint32_t mapId,
    TerrainFacetCells cells,const MovementFacetRequest& request,uint32_t flags,
    const TerrainLiquidMaterials& materials,FacetNormalization normalization,
    std::vector<MovementFacet>& output) {
    if (!chunk.loaded) return MovementGeometryStatus::NotReady;
    if (!chunk.hasHeights()) return MovementGeometryStatus::Failed;
    const auto append=[&](std::vector<MovementFacet> facets) {
        output.insert(output.end(),std::make_move_iterator(facets.begin()),std::make_move_iterator(facets.end()));
    };
    if (chunk.flags&2)
        append(queryImpassableTerrainFacets(chunk.boundsMin,chunk.boundsMax,request.queryMin,request.queryMax));
    cells.minX=std::max(0,cells.minX-globalX*8);cells.minY=std::max(0,cells.minY-globalY*8);
    cells.maxX=std::min(7,cells.maxX-globalX*8);cells.maxY=std::min(7,cells.maxY-globalY*8);
    const glm::vec3 low=request.queryMin-chunk.originalCorner,high=request.queryMax-chunk.originalCorner;
    if (flags&0x100) {
        auto facets=queryTerrainFacets(chunk.heightMap.heights,chunk.holes,chunk.originalCorner,cells,low,high,normalization);
        if (!facets) return MovementGeometryStatus::Failed;
        append(std::move(*facets));
    }
    if (flags&0x30000) {
        const bool fishableOnly=(flags&0x10000) && !(flags&0x20000);
        for (const auto& layer:chunk.liquids) {
            uint16_t type=layer.liquidType;
            // Original CreateLiquids remaps legacy Outland magma to row 15.
            if (layer.legacyTiles && mapId==530 && type==3) type=15;
            std::optional<TerrainLiquidMaterial> material;
            if (fishableOnly || !layer.vertexFormat) {
                material=materials.find(type);
                if (!material) return MovementGeometryStatus::Failed;
            }
            if (fishableOnly && !(material->flags&4)) continue;
            const uint16_t format=layer.vertexFormat ? *layer.vertexFormat : material->vertexFormat;
            const TerrainFacetCells bounds{layer.minX,layer.minY,layer.maxX,layer.maxY};
            const auto vertices=makeTerrainLiquidVertices(globalX,globalY,bounds,chunk.originalCorner.z,layer.heights,format);
            TerrainLiquidGrid grid{bounds,vertices,layer.cellMask,{},type};
            if (layer.legacyTiles) grid.legacyTiles=*layer.legacyTiles;
            auto result=queryTerrainLiquidFacets(grid,cells,chunk.originalCorner,low.z,high.z,normalization);
            if (!result) return MovementGeometryStatus::Failed;
            append(std::move(result->facets));
        }
    }
    return MovementGeometryStatus::Ready;
}
} // namespace wowee::collision
