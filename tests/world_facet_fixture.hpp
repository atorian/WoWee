#pragma once
#include "collision/world_facets.hpp"
#include "pipeline/wmo_loader.hpp"
#include <istream>
#include <memory>

namespace world_fixture {
using namespace wowee;
struct Settings {
    bool dungeon=false,provider=false,available=false,impassable=false,flight=false;
    unsigned wmo=0,area=0;
    uint32_t doodadFlags=0,entityFlags=0,tag=0;
    uint16_t holes=0;
    uint64_t owner=0;
    bool read(std::istream& in) {
        return bool(in>>dungeon>>wmo>>area>>doodadFlags>>owner>>entityFlags>>provider>>available>>impassable>>holes>>flight>>tag)
            &&wmo<=5&&area<=5;
    }
};
struct Materials final:collision::TerrainLiquidMaterials,collision::WmoLiquidMaterials {
    std::optional<collision::TerrainLiquidMaterial> find(uint16_t) const override {return {};}
    std::optional<uint32_t> flags(uint16_t type) const override {return type==1?std::optional<uint32_t>(4):std::nullopt;}
};
struct Provider final:collision::EntityFacetProvider {
    bool available=false;uint32_t calls=0;collision::EntityFacetData data;
    std::optional<collision::EntityFacetData> query(uint64_t&,uint32_t,uint32_t) override {
        ++calls;return available?std::optional(data):std::nullopt;
    }
};
inline collision::M2Shape mesh(float z) {
    collision::M2Shape s;s.vertices={{-10,-40,z},{0,-40,z},{-5,-30,z}};s.indices={0,1,2};s.faceNormals={{0,0,1}};return s;
}
// Own every object referenced by the supplied world, including cross-list tags.
// The model and link scene is shared by the query and movement front doors.
struct Scene {
    collision::M2Shape shape=mesh(2),entityShape=mesh(4);
    const glm::vec3 low{-10,-40,-5},high{0,-30,5};
    collision::DoodadFacetModel doodadModel{low,high,&shape};
    collision::DoodadFacetPlacement doodad;
    collision::EntityFacetPlacement entity;
    std::array<collision::DoodadFacetPlacement*,1> doodads{&doodad};
    std::array<collision::EntityFacetPlacement*,1> entities{&entity};
    collision::WorldFacetLinks links{doodads,entities};
    Provider provider;
    Materials materials;
    collision::WmoShape model;
    std::array<collision::WmoWorldGroup,1> groups{{{0,{low,high},7}}};
    collision::WmoWorldPlacement placement;
    std::unique_ptr<collision::WorldFacetView> world=std::make_unique<collision::WorldFacetView>();
    std::shared_ptr<const collision::TerrainTileHeights> tile;
    std::array<collision::WorldFacetLinks,256> chunkLinks{};
    std::unique_ptr<collision::WorldMovementFacets> source;
    explicit Scene(const Settings& s) {
        doodad.flags=s.doodadFlags;doodad.owner=s.owner;doodad.model=&doodadModel;doodad.boundsMin=low;doodad.boundsMax=high;
        entity.staticFlags=s.entityFlags;entity.owner=0xf110000012345678;entity.parameter=7;
        provider.available=s.available;provider.data={&entityShape,low,high,glm::mat4(1)};
        pipeline::WMOModel input;input.groups.resize(1);input.groupInfo.resize(1);auto& g=input.groups[0];
        g.loadState=s.wmo==4?pipeline::WmoGroupLoadState::NotLoaded:pipeline::WmoGroupLoadState::Ready;
        g.boundingBoxMin=input.groupInfo[0].boundingBoxMin=low;g.boundingBoxMax=input.groupInfo[0].boundingBoxMax=high;
        g.vertices.resize(3);const auto wmoMesh=mesh(3);for(size_t i=0;i<3;++i)g.vertices[i].position=wmoMesh.vertices[i];
        g.indices={0,1,2};g.triFlags={8};std::string error;
        g.collisionBsp=pipeline::WmoBspData::build({{4,0xffff,0xffff,1,0,0}},{0},g.indices,3,error);
        g.liquid.xVerts=g.liquid.yVerts=2;g.liquid.xTiles=g.liquid.yTiles=1;g.liquid.liquidType=1;
        g.liquid.basePosition={-5,-35,0};g.liquid.heights={1,1,1,1};g.liquid.flags={0};
        model.boundingBoxMin=low;model.boundingBoxMax=high;
        model.originalGroups.push_back(collision::WmoModelGroup::build(input,0,collision::WmoGroupShape::build(g)));
        placement.model=&model;placement.groupsReady=s.wmo!=3;placement.flags=s.wmo==2?0x100:0;placement.bounds={low,high};placement.owner=100;
        if(s.wmo!=5)placement.groups=groups;
        if(s.wmo)world->buildings={&placement,1};world->buildingGroups.emplace(7,links);
        pipeline::ADTTerrain terrain;
        for(int i=0;i<2;++i) {
            if(s.area==unsigned(4+i))continue;
            auto& c=terrain.chunks[i];c.loaded=true;c.heightMap.loaded=true;c.heightMap.heights.fill(float(i));c.holes=s.holes;c.flags=s.impassable?2:0;
        }
        if(s.flight){terrain.flightBounds.emplace();for(int i=0;i<18;++i)(*terrain.flightBounds)[i]=i<9?4:-4;}
        tile=collision::TerrainTileHeights::build(terrain,32,32);
        chunkLinks[0]=links;chunkLinks[1]=links;
        auto& area=world->areas[32+32*64];area.hasTerrain=s.area!=0;area.terrain=tile.get();area.chunks=chunkLinks;
        area.state=s.area>=3?collision::MovementGeometryStatus::Ready:collision::MovementGeometryStatus::NotReady;
        source=std::make_unique<collision::WorldMovementFacets>(*world,0,s.dungeon,materials,materials,
            s.provider?&provider:nullptr,collision::FacetNormalization::X87,s.tag);
    }
    Scene(const Scene&)=delete;
    Scene& operator=(const Scene&)=delete;
};
} // namespace world_fixture
