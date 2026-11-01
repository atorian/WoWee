#include "world_facet_fixture.hpp"
#include "pipeline/wmo_loader.hpp"
#include "movement_facet_probe_output.hpp"
#include <iomanip>
#include <memory>
using namespace wowee;
int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    std::string name;const auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
    while(std::cin>>name) {
        world_fixture::Settings settings;size_t calls;
        collision::MovementFacetRequest request{};
        if(!settings.read(std::cin)||!(std::cin>>calls)||calls>8||!read(request.bodyMin)||!read(request.bodyMax)||!read(request.queryMin)||!read(request.queryMax))return 2;
        world_fixture::Scene scene(settings);auto& source=*scene.source;
        auto& d=scene.doodad;auto& e=scene.entity;auto& provider=scene.provider;
        std::vector<collision::MovementFacet> facets;
        std::cout<<"{\"name\":\""<<name<<"\",\"results\":[";
        for(size_t i=0;i<calls;++i) {
            uint32_t flags;if(!(std::cin>>flags))return 2;const auto result=source.query(request,flags,facets);
            if(result==collision::MovementGeometryStatus::Failed)return 3;
            if(i)std::cout<<',';std::cout<<"{\"ready\":"<<(result==collision::MovementGeometryStatus::Ready?"true":"false")<<",\"status\":"<<source.originalStatusFlags()
                <<",\"tag\":"<<source.queryTag()<<",\"doodad_tag\":"<<d.queryTag<<",\"entity_tag\":"<<e.queryTag<<",\"callbacks\":"<<provider.calls<<",\"facets\":[";
            facet_probe::writeFacets(facets);std::cout<<"]}";
        }
        std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
