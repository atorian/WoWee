#include "wmo_model_fixture.hpp"
#include "movement_facet_probe_output.hpp"
using namespace wmo_fixture;
using namespace wowee;
int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    std::string name;
    const auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
    while (std::cin>>name) {
        bool sse;uint32_t flags;uint64_t owner;size_t count;glm::vec3 low,high;glm::mat4 matrix;
        if (!(std::cin>>sse>>flags>>owner>>count) || count>128 || !read(low) || !read(high)) return 2;
        for (int c=0;c<4;++c) for(int r=0;r<4;++r) if (!(std::cin>>matrix[c][r])) return 2;
        collision::WmoShape model;Materials materials;
        if(!readModel(count,model,materials))return 2;
        std::vector<collision::MovementFacet> facets;
        const auto result=collision::appendWmoModelFacets(model,low,high,flags,matrix,owner,materials,
            sse?collision::FacetNormalization::NativeSse:collision::FacetNormalization::X87,facets);
        if(result.status!=collision::MovementGeometryStatus::Ready)return 3;
        std::cout<<"{\"name\":\""<<name<<"\",\"status\":"<<result.originalStatusFlags<<",\"diagnostics\":"<<result.degenerateFaces<<",\"facets\":[";
        facet_probe::writeFacets(facets);
        std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
