#include "collision/terrain_facets.hpp"
#include "collision/liquid_facets.hpp"
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const bool liquidVertices = argc == 2 && std::string(argv[1]) == "--liquid-vertices";
    const bool flight = argc == 2 && std::string(argv[1]) == "--flight";
    const bool liquid = argc == 2 && std::string(argv[1]) == "--liquid";
    const bool impassable = argc == 2 && std::string(argv[1]) == "--impassable";
    std::cout<<std::setprecision(9)<<std::showpoint;
    std::string name;
    while (std::cin>>name) {
        bool sse;
        uint16_t holes;
        wowee::collision::TerrainFacetCells cells;
        glm::vec3 corner,low,high;
        auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
        std::optional<std::vector<wowee::collision::MovementFacet>> facets;
        uint32_t diagnostics=0;
        if (liquidVertices) {
            int chunkX,chunkY;float baseZ;uint16_t format;
            if (!(std::cin>>chunkX>>chunkY>>cells.minX>>cells.minY>>cells.maxX>>cells.maxY>>baseZ>>format)) return 2;
            const int count=(cells.maxX-cells.minX+1)*(cells.maxY-cells.minY+1);
            if (count<1 || count>81) return 2;
            std::vector<float> heights(count);
            for (auto& h:heights) if (!(std::cin>>h)) return 2;
            const auto vertices=wowee::collision::makeTerrainLiquidVertices(chunkX,chunkY,cells,baseZ,heights,format);
            std::cout<<"{\"name\":\""<<name<<"\",\"vertices\":[";
            for (size_t i=0;i<vertices.size();++i) {
                if(i)std::cout<<',';
                const auto& v=vertices[i];std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']';
            }
            std::cout<<"]}\n";
            continue;
        }
        if (flight) {
            std::array<int16_t,18> heights;
            if (!read(corner) || !read(low) || !read(high)) return 2;
            for (auto& h:heights) if (!(std::cin>>h)) return 2;
            facets=wowee::collision::queryFlightBoundsFacets(heights,corner,low.z,high.z);
        } else if (liquid) {
            wowee::collision::TerrainLiquidGrid grid;
            auto& b=grid.bounds;
            size_t count,legacyCount;
            if (!(std::cin>>sse>>b.minX>>b.minY>>b.maxX>>b.maxY>>grid.cellMask>>grid.liquidType
                >>cells.minX>>cells.minY>>cells.maxX>>cells.maxY) || !read(corner) || !read(low) || !read(high)
                || !(std::cin>>count) || count>81) return 2;
            std::vector<glm::vec3> vertices(count);
            for (auto& v:vertices) if (!read(v)) return 2;
            if (!(std::cin>>legacyCount) || (legacyCount && legacyCount!=64)) return 2;
            std::vector<uint8_t> tiles(legacyCount);
            for (auto& t:tiles) { unsigned value; if (!(std::cin>>value) || value>255) return 2; t=uint8_t(value); }
            grid.vertices=vertices; grid.legacyTiles=tiles;
            auto result=wowee::collision::queryTerrainLiquidFacets(grid,cells,corner,low.z,high.z,
                sse ? wowee::collision::FacetNormalization::NativeSse : wowee::collision::FacetNormalization::X87);
            if (!result) return 3;
            diagnostics=result->degenerateFaces;
            facets=std::move(result->facets);
        } else if (impassable) {
            glm::vec3 chunkMin,chunkMax;
            if (!read(chunkMin) || !read(chunkMax) || !read(low) || !read(high)) return 2;
            facets = wowee::collision::queryImpassableTerrainFacets(chunkMin,chunkMax,low,high);
        } else {
            if (!(std::cin>>sse>>holes>>cells.minX>>cells.minY>>cells.maxX>>cells.maxY) ||
                cells.minX<0 || cells.minY<0 || cells.maxX>7 || cells.maxY>7 ||
                !read(corner) || !read(low) || !read(high)) return 2;
            std::array<float,145> heights;
            for (auto& h:heights) if (!(std::cin>>h)) return 2;
            facets=wowee::collision::queryTerrainFacets(heights,holes,corner,cells,low,high,
                sse ? wowee::collision::FacetNormalization::NativeSse : wowee::collision::FacetNormalization::X87);
        }
        if (!facets) return 3;
        std::cout<<"{\"name\":\""<<name<<"\",\"diagnostics\":"<<diagnostics<<",\"facets\":[";
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
