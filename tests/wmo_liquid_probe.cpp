#include "collision/liquid_facets.hpp"
#include <iomanip>
#include <iostream>
#include <string>
using namespace wowee::collision;
int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    std::string name;
    while(std::cin>>name) {
        bool sse;WmoLiquidGrid grid{};glm::vec3 low,high;glm::mat4 matrix;uint32_t flags;uint64_t owner;
        auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
        if (!(std::cin>>sse>>grid.xTiles>>grid.yTiles) || grid.xTiles>200 || grid.yTiles>200 || !read(grid.corner) || !read(low) || !read(high)
            || !(std::cin>>flags>>grid.liquidFlags>>grid.liquidType>>owner)) return 2;
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!(std::cin>>matrix[c][r])) return 2;
        grid.xVerts=grid.xTiles+1;grid.yVerts=grid.yTiles+1;
        std::vector<float> heights(grid.xVerts*grid.yVerts);std::vector<uint8_t> tiles(grid.xTiles*grid.yTiles);
        for(auto& h:heights)if(!(std::cin>>h))return 2;
        for(auto& t:tiles){unsigned v;if(!(std::cin>>v)||v>255)return 2;t=uint8_t(v);}
        const auto vertices=makeWmoLiquidVertices(grid.xVerts,grid.yVerts,grid.corner,heights);grid.vertices=vertices;grid.tiles=tiles;
        const auto result=queryWmoLiquidFacets(grid,low,high,flags,matrix,owner,sse?FacetNormalization::NativeSse:FacetNormalization::X87);
        if(!result)return 3;
        std::cout<<"{\"name\":\""<<name<<"\",\"diagnostics\":"<<result->degenerateFaces<<",\"facets\":[";
        for(size_t i=0;i<result->facets.size();++i) {
            if(i)std::cout<<',';const auto& f=result->facets[i];
            std::cout<<"{\"owner\":"<<f.owner<<",\"plane\":["<<f.plane.normal.x<<','<<f.plane.normal.y<<','<<f.plane.normal.z<<','<<f.plane.offset<<"],\"vertices\":[";
            for(size_t j=0;j<3;++j){if(j)std::cout<<',';const auto& v=f.vertices[j];std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']';}
            std::cout<<"]}";
        }
        std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
