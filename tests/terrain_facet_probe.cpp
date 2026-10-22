#include "collision/terrain_facets.hpp"
#include <iomanip>
#include <iostream>
#include <string>

int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    std::string name;
    while (std::cin>>name) {
        bool sse;
        uint16_t holes;
        wowee::collision::TerrainFacetCells cells;
        glm::vec3 corner,low,high;
        auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
        if (!(std::cin>>sse>>holes>>cells.minX>>cells.minY>>cells.maxX>>cells.maxY) ||
            cells.minX<0 || cells.minY<0 || cells.maxX>7 || cells.maxY>7 ||
            !read(corner) || !read(low) || !read(high)) return 2;
        std::array<float,145> heights;
        for (auto& h:heights) if (!(std::cin>>h)) return 2;
        auto facets=wowee::collision::queryTerrainFacets(heights,holes,corner,cells,low,high,
            sse ? wowee::collision::FacetNormalization::NativeSse : wowee::collision::FacetNormalization::X87);
        if (!facets) return 3;
        std::cout<<"{\"name\":\""<<name<<"\",\"diagnostics\":0,\"facets\":[";
        for (size_t i=0;i<facets->size();++i) {
            if (i)std::cout<<',';
            const auto& f=(*facets)[i];
            std::cout<<"{\"owner\":0,\"plane\":["<<f.plane.normal.x<<','<<f.plane.normal.y<<','
                     <<f.plane.normal.z<<','<<f.plane.offset<<"],\"vertices\":[";
            for (size_t j=0;j<3;++j) {
                if(j)std::cout<<',';
                const auto& v=f.vertices[j];std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']';
            }
            std::cout<<"]}";
        }
        std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
