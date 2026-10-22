#include "collision/terrain_chunk_facets.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
using namespace wowee::collision;
struct Materials final : TerrainLiquidMaterials {
    std::unordered_map<uint16_t,TerrainLiquidMaterial> values;
    std::optional<TerrainLiquidMaterial> find(uint16_t type) const override {
        const auto it=values.find(type);
        return it==values.end()?std::nullopt:std::optional(it->second);
    }
};
int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    std::string name;
    while(std::cin>>name) {
        wowee::pipeline::ADTTerrain terrain;
        int x,y;float base;uint32_t mapId,flags;bool impassable;uint16_t holes;
        TerrainFacetCells cells;MovementFacetRequest request{};
        if (!(std::cin>>x>>y>>base>>mapId>>holes>>impassable>>flags>>cells.minX>>cells.minY>>cells.maxX>>cells.maxY)
            || x<0 || x>1023 || y<0 || y>1023) return 2;
        auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
        if (!read(request.queryMin) || !read(request.queryMax)) return 2;
        const int index=(y&15)*16+(x&15);auto& chunk=terrain.chunks[index];
        chunk.loaded=true;chunk.heightMap.loaded=true;chunk.position[2]=base;
        chunk.flags=impassable?2:0;chunk.holes=holes;
        for(auto& h:chunk.heightMap.heights) if (!(std::cin>>h)) return 2;
        size_t count;if (!(std::cin>>count) || count>2) return 2;
        Materials materials;
        for(size_t i=0;i<count;++i) {
            wowee::pipeline::ADTTerrain::CollisionLiquidLayer layer;
            bool legacy;unsigned minX,minY,maxX,maxY;uint16_t format;uint32_t liquidFlags;
            if (!(std::cin>>legacy>>layer.liquidType>>liquidFlags>>format>>minX>>minY>>maxX>>maxY>>layer.cellMask)
                || minX>=maxX || minY>=maxY || maxX>8 || maxY>8) return 2;
            layer.minX=uint8_t(minX);layer.minY=uint8_t(minY);layer.maxX=uint8_t(maxX);layer.maxY=uint8_t(maxY);
            layer.heights.resize((maxX-minX+1)*(maxY-minY+1));
            for(auto& h:layer.heights) if (!(std::cin>>h)) return 2;
            if(legacy) {
                layer.legacyTiles.emplace();
                for(auto& t:*layer.legacyTiles) { unsigned value;if (!(std::cin>>value) || value>255) return 2;t=uint8_t(value); }
            } else layer.vertexFormat=format;
            const auto type=legacy && mapId==530 && layer.liquidType==3 ? 15 : layer.liquidType;
            materials.values[uint16_t(type)]={liquidFlags,format};
            terrain.collisionLiquids[index].push_back(std::move(layer));
        }
        const auto tile=TerrainTileHeights::build(terrain,x>>4,y>>4);
        std::vector<MovementFacet> facets;
        const auto result=appendTerrainChunkFacets(tile->chunks[index],x,y,mapId,cells,request,flags,materials,FacetNormalization::X87,facets);
        std::cout<<"{\"name\":\""<<name<<"\",\"ready\":"<<(result==MovementGeometryStatus::Ready ? "true":"false")<<",\"facets\":[";
        for(size_t i=0;i<facets.size();++i) {
            if(i)std::cout<<',';const auto& f=facets[i];
            std::cout<<"{\"owner\":0,\"plane\":["<<f.plane.normal.x<<','<<f.plane.normal.y<<','<<f.plane.normal.z<<','<<f.plane.offset<<"],\"vertices\":[";
            for(size_t j=0;j<3;++j) {if(j)std::cout<<',';const auto& v=f.vertices[j];std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']';}
            std::cout<<"]}";
        }
        std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
