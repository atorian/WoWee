#pragma once
#include "collision/wmo_model_facets.hpp"
#include "pipeline/wmo_loader.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
namespace wmo_fixture {
using namespace wowee;
class Materials final:public collision::WmoLiquidMaterials {
public:
    std::unordered_map<uint16_t,uint32_t> rows;
    std::optional<uint32_t> flags(uint16_t type) const override {
        auto i=rows.find(type);return i==rows.end()?std::nullopt:std::optional(i->second);
    }
};
inline bool readModel(size_t count,collision::WmoShape& model,Materials& materials) {
    const auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
    pipeline::WMOModel input;input.groups.resize(count);input.groupInfo.resize(count);

        for (size_t i=0;i<count;++i) {
            auto& g=input.groups[i];auto& info=input.groupInfo[i];bool ready,liquid;size_t verts,tris,faces;
            if (!(std::cin>>ready>>g.flags) || !read(info.boundingBoxMin) || !read(info.boundingBoxMax) ||
                !read(g.boundingBoxMin) || !read(g.boundingBoxMax) || !(std::cin>>verts>>tris>>faces) || verts>65536 || tris>65535 || faces>65535) return false;
            g.groupId=uint32_t(i);g.loadState=ready?pipeline::WmoGroupLoadState::Ready:pipeline::WmoGroupLoadState::NotLoaded;
            g.vertices.resize(verts);g.indices.resize(tris*3);g.triFlags.resize(tris);
            std::vector<glm::vec3> vertices(verts);
            for(size_t v=0;v<verts;++v) { if(!read(vertices[v]))return false;g.vertices[v].position=vertices[v]; }
            for(auto& v:g.indices) if(!(std::cin>>v))return false;
            for(auto& f:g.triFlags) { unsigned n;if(!(std::cin>>n)||n>255)return false;f=uint8_t(n); }
            std::vector<uint16_t> refs(faces);for(auto& r:refs)if(!(std::cin>>r))return false;
            std::string error;
            g.collisionBsp=pipeline::WmoBspData::build({{4,0xffff,0xffff,uint16_t(faces),0,0}},std::move(refs),g.indices,verts,error);
            if(!g.collisionBsp)return false;
            if(!(std::cin>>liquid))return false;
            if(liquid) {
                auto& l=g.liquid;uint32_t material;
                if(!(std::cin>>l.xTiles>>l.yTiles>>l.liquidType>>material)||!read(l.basePosition))return false;
                l.xVerts=l.xTiles+1;l.yVerts=l.yTiles+1;
                if(size_t(l.xVerts)*l.yVerts>65536)return false;
                l.heights.resize(size_t(l.xVerts)*l.yVerts);l.flags.resize(size_t(l.xTiles)*l.yTiles);
                for(auto& h:l.heights)if(!(std::cin>>h))return false;
                for(auto& f:l.flags){unsigned n;if(!(std::cin>>n)||n>255)return false;f=uint8_t(n);}
                materials.rows[l.liquidType]=material;
            }
            model.originalGroups.push_back(collision::WmoModelGroup::build(input,i,collision::WmoGroupShape::build(g)));
        }
    return true;
}
} // namespace wmo_fixture
