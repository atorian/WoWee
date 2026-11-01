#include "collision/doodad_facets.hpp"
#include "movement_facet_probe_output.hpp"
#include <iomanip>
#include <unordered_map>
using namespace wowee;
int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    const auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
    std::string name;
    while(std::cin>>name) {
        size_t count,calls;if(!(std::cin>>count>>calls)||count>128||calls>128)return 2;
        std::vector<collision::DoodadFacetPlacement> objects(count);
        std::vector<collision::DoodadFacetModel> models(count);
        std::vector<collision::M2Shape> shapes(count);
        for(size_t i=0;i<count;++i) {
            auto& p=objects[i];auto& m=models[i];auto& s=shapes[i];bool present;size_t verts,tris;
            if(!(std::cin>>p.flags>>p.staticFlags>>p.queryTag>>p.owner>>present>>verts>>tris)||verts>65536||tris>65535||
                !read(p.boundsMin)||!read(p.boundsMax)||!read(m.boundsMin)||!read(m.boundsMax))return 2;
            for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!(std::cin>>p.transform[c][r]))return 2;
            s.vertices.resize(verts);s.indices.resize(tris*3);s.faceNormals.resize(tris);
            for(auto& v:s.vertices)if(!read(v))return 2;
            for(auto& v:s.indices)if(!(std::cin>>v)||v>=verts)return 2;
            for(auto& v:s.faceNormals)if(!read(v))return 2;
            p.model=present?&m:nullptr;m.shape=tris?&s:nullptr;
        }
        std::vector<collision::MovementFacet> facets;
        for(size_t i=0;i<calls;++i) {
            uint32_t flags,tag;size_t countLinks;collision::MovementFacetRequest r{};
            if(!(std::cin>>flags>>tag>>countLinks)||countLinks>1024||!read(r.queryMin)||!read(r.queryMax))return 2;
            std::vector<collision::DoodadFacetPlacement*> links;
            for(size_t j=0;j<countLinks;++j){size_t index;if(!(std::cin>>index)||index>=objects.size())return 2;links.push_back(&objects[index]);}
            collision::appendDoodadFacets(links,r,flags,tag,facets);
        }
        std::cout<<"{\"name\":\""<<name<<"\",\"state\":[";
        for(size_t i=0;i<objects.size();++i){if(i)std::cout<<',';std::cout<<"{\"tag\":"<<objects[i].queryTag<<",\"static\":"<<objects[i].staticFlags<<'}';}
        std::cout<<"],\"facets\":[";facet_probe::writeFacets(facets);std::cout<<"],\"diagnostics\":0}\n";
    }
    return std::cin.eof()?0:2;
}
