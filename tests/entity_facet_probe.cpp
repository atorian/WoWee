#include "collision/entity_facets.hpp"
#include "movement_facet_probe_output.hpp"
#include <iomanip>
#include <unordered_map>
using namespace wowee;
struct Provider final:collision::EntityFacetProvider {
    struct Entry { bool available;uint64_t owner;collision::EntityFacetData data; };
    struct Call {uint64_t owner;uint32_t parameter,flags;};
    std::unordered_map<uint32_t,Entry> entries;
    std::vector<Call> calls;
    std::optional<collision::EntityFacetData> query(uint64_t& owner,uint32_t parameter,uint32_t flags) override {
        calls.push_back({owner,parameter,flags});const auto& e=entries.at(parameter);
        if(!e.available)return {};owner=e.owner;return e.data;
    }
};
int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    const auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
    std::string name;
    while(std::cin>>name) {
        size_t count,calls;if(!(std::cin>>count>>calls)||count>128||calls>128)return 2;
        std::vector<collision::EntityFacetPlacement> objects(count);std::vector<collision::M2Shape> shapes(count);Provider provider;
        for(size_t i=0;i<count;++i) {
            auto& p=objects[i];auto& s=shapes[i];Provider::Entry e{};size_t verts,tris;
            if(!(std::cin>>p.staticFlags>>p.queryTag>>p.owner>>p.parameter>>e.available>>e.owner>>verts>>tris)||verts>65536||tris>65535||
                !read(e.data.boundsMin)||!read(e.data.boundsMax))return 2;
            for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!(std::cin>>e.data.transform[c][r]))return 2;
            s.vertices.resize(verts);s.indices.resize(tris*3);s.faceNormals.resize(tris);
            for(auto& v:s.vertices)if(!read(v))return 2;
            for(auto& v:s.indices)if(!(std::cin>>v)||v>=verts)return 2;
            for(auto& v:s.faceNormals)if(!read(v))return 2;
            e.data.shape=tris?&s:nullptr;provider.entries[p.parameter]=e;
        }
        std::vector<collision::MovementFacet> facets;
        for(size_t i=0;i<calls;++i) {
            uint32_t flags,tag;size_t countLinks;bool enabled;collision::MovementFacetRequest r{};
            if(!(std::cin>>flags>>tag>>enabled>>countLinks)||countLinks>1024||!read(r.queryMin)||!read(r.queryMax))return 2;
            std::vector<collision::EntityFacetPlacement*> links;
            for(size_t j=0;j<countLinks;++j){size_t index;if(!(std::cin>>index)||index>=objects.size())return 2;links.push_back(&objects[index]);}
            collision::appendEntityFacets(links,enabled?&provider:nullptr,r,flags,tag,facets);
        }
        std::cout<<"{\"name\":\""<<name<<"\",\"tags\":[";
        for(size_t i=0;i<objects.size();++i){if(i)std::cout<<',';std::cout<<objects[i].queryTag;}
        std::cout<<"],\"calls\":[";
        for(size_t i=0;i<provider.calls.size();++i){if(i)std::cout<<',';const auto& c=provider.calls[i];std::cout<<"{\"owner\":"<<c.owner<<",\"parameter\":"<<c.parameter<<",\"flags\":"<<c.flags<<'}';}
        std::cout<<"],\"facets\":[";facet_probe::writeFacets(facets);std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
