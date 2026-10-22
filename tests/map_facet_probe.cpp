#include "collision/map_facets.hpp"
#include <iostream>
#include <iomanip>
#include <string>

using namespace wowee::collision;
namespace {
struct Call { int kind,x,y; TerrainFacetCells cells; uint32_t flags; };
class SuppliedMap final : public MapMovementFacetSource {
public:
    SuppliedMap(bool dungeon,int failed) : MapMovementFacetSource(dungeon),failed_(failed) {}
    std::vector<Call> calls;
private:
    int failed_;
    MovementGeometryStatus record(Call call,std::vector<MovementFacet>& output) {
        const auto index=int(calls.size());calls.push_back(call);
        output.emplace_back();
        return index==failed_ ? MovementGeometryStatus::NotReady : MovementGeometryStatus::Ready;
    }
    MovementGeometryStatus queryMapObjects(const MovementFacetRequest&,uint32_t flags,
                                           std::vector<MovementFacet>& output) override {
        return record({0,0,0,{},flags},output);
    }
    MovementGeometryStatus queryChunk(int x,int y,TerrainFacetCells cells,const MovementFacetRequest&,
                                      uint32_t flags,std::vector<MovementFacet>& output) override {
        return record({1,x,y,cells,flags},output);
    }
    MovementGeometryStatus queryFlightBounds(int x,int y,const MovementFacetRequest&,
                                             std::vector<MovementFacet>& output) override {
        return record({2,x,y,{},0},output);
    }
};
}
int main(int argc,char** argv) {
    const bool unavailable=argc==2 && std::string(argv[1])=="--unavailable";
    std::cout<<std::setprecision(9)<<std::showpoint;
    std::string name;
    while (std::cin>>name) {
        if (unavailable) {
            int x,y;bool terrain;uint32_t flags;MovementFacetRequest r{};
            auto read=[](glm::vec3& p) { return bool(std::cin>>p.x>>p.y>>p.z); };
            if (!(std::cin>>x>>y>>terrain>>flags) || !read(r.bodyMin) || !read(r.bodyMax) ||
                !read(r.queryMin) || !read(r.queryMax)) return 2;
            std::vector<MovementFacet> facets;
            const auto result=appendUnavailableTerrainFacets(x,y,terrain,r,flags,facets);
            std::cout<<"{\"name\":\""<<name<<"\",\"ready\":"
                     <<(result==MovementGeometryStatus::Ready ? "true" : "false")<<",\"facets\":[";
            for (size_t i=0;i<facets.size();++i) {
                if(i)std::cout<<',';
                const auto& f=facets[i];
                std::cout<<"{\"owner\":"<<f.owner<<",\"plane\":["<<f.plane.normal.x<<','<<f.plane.normal.y<<','
                         <<f.plane.normal.z<<','<<f.plane.offset<<"],\"vertices\":[";
                for (size_t j=0;j<3;++j) {
                    if(j)std::cout<<',';
                    const auto& v=f.vertices[j];std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']';
                }
                std::cout<<"]}";
            }
            std::cout<<"]}\n";
            continue;
        }
        bool dungeon;int failed;uint32_t flags;MovementFacetRequest request{};
        auto read=[](glm::vec3& p){return bool(std::cin>>p.x>>p.y>>p.z);};
        if (!(std::cin>>dungeon>>failed>>flags) || !read(request.queryMin) || !read(request.queryMax)) return 2;
        SuppliedMap map(dungeon,failed);
        std::vector<MovementFacet> facets(4);
        const auto status=map.query(request,flags,facets);
        std::cout<<"{\"name\":\""<<name<<"\",\"ready\":"<<(status==MovementGeometryStatus::Ready ? "true" : "false")
                 <<",\"facets\":"<<facets.size()<<",\"calls\":[";
        for (size_t i=0;i<map.calls.size();++i) {
            if(i)std::cout<<',';
            const auto& c=map.calls[i];
            std::cout<<'['<<c.kind<<','<<c.x<<','<<c.y<<','<<c.cells.minX<<','<<c.cells.minY<<','
                     <<c.cells.maxX<<','<<c.cells.maxY<<','<<c.flags<<']';
        }
        std::cout<<"]}\n";
    }
    return std::cin.eof() ? 0 : 2;
}
