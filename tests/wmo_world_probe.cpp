#include "collision/wmo_world_facets.hpp"
#include "wmo_model_fixture.hpp"
#include "movement_facet_probe_output.hpp"
using namespace wowee;
using namespace wmo_fixture;
class Links final:public collision::WmoLinkedFacets {
public:
    struct Call { bool kind;uint64_t link;uint32_t flags;size_t count;collision::MovementFacetRequest request; };
    std::unordered_map<uint64_t,bool> ready;
    std::vector<Call> calls;
    collision::MovementGeometryStatus doodads(uint64_t link,const collision::MovementFacetRequest& r,
        uint32_t flags,std::vector<collision::MovementFacet>& out) override {
        calls.push_back({false,link,flags,out.size(),r});
        return ready.at(link)?collision::MovementGeometryStatus::Ready:collision::MovementGeometryStatus::NotReady;
    }
    collision::MovementGeometryStatus destructibles(uint64_t link,const collision::MovementFacetRequest& r,
        uint32_t flags,std::vector<collision::MovementFacet>& out) override {
        calls.push_back({true,link,flags,out.size(),r});
        return collision::MovementGeometryStatus::Ready;
    }
};
int main() {
    std::cout<<std::setprecision(9)<<std::showpoint;
    const auto read=[](glm::vec3& v){return bool(std::cin>>v.x>>v.y>>v.z);};
    const auto box=[&](collision::WmoGroupBounds& b){return read(b.low)&&read(b.high);};
    const auto matrix=[](glm::mat4& m){for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!(std::cin>>m[c][r]))return false;return true;};
    std::string name;
    while(std::cin>>name) {
        bool sse;uint32_t flags,status;size_t count,proxyCount;collision::MovementFacetRequest r{};
        if(!(std::cin>>sse>>flags>>status>>count>>proxyCount)||count>128||proxyCount>128||
            !read(r.bodyMin)||!read(r.bodyMax)||!read(r.queryMin)||!read(r.queryMax))return 2;
        std::vector<collision::DestructibleCollisionProxy> proxies(proxyCount);
        for(auto& p:proxies)if(!read(p.center)||!(std::cin>>p.radius))return 2;
        std::vector<collision::WmoShape> models(count);
        std::vector<collision::WmoWorldPlacement> placements(count);
        std::vector<std::vector<collision::WmoWorldGroup>> groups(count);
        Materials materials;Links links;
        for(size_t i=0;i<count;++i) {
            auto& p=placements[i];auto& model=models[i];bool present;size_t groupCount,linkCount;
            if(!(std::cin>>p.flags>>present>>p.groupsReady>>p.owner>>groupCount>>linkCount)||groupCount>128||linkCount>128||
                !box(p.bounds)||!read(model.boundingBoxMin)||!read(model.boundingBoxMax)||!matrix(p.transform)||!matrix(p.inverse))return 2;
            if(!readModel(groupCount,model,materials))return 2;
            p.model=present?&model:nullptr;groups[i].resize(linkCount);
            for(auto& g:groups[i]) {
                bool ready;
                if(!(std::cin>>g.index>>g.link>>ready)||!box(g.bounds))return 2;
                links.ready[g.link]=ready;
            }
            p.groups=groups[i];
        }
        std::vector<collision::MovementFacet> facets;
        const auto result=collision::appendWorldWmoFacets(placements,proxies,r,flags,materials,links,
            sse?collision::FacetNormalization::NativeSse:collision::FacetNormalization::X87,facets);
        if(result.status==collision::MovementGeometryStatus::Failed)return 3;
        std::cout<<"{\"name\":\""<<name<<"\",\"ready\":"<<(result.status==collision::MovementGeometryStatus::Ready?"true":"false")
            <<",\"status\":"<<(status|result.originalStatusFlags)<<",\"diagnostics\":"<<result.degenerateFaces<<",\"calls\":[";
        for(size_t i=0;i<links.calls.size();++i) {
            if(i)std::cout<<',';const auto& c=links.calls[i];
            std::cout<<"{\"kind\":"<<c.kind<<",\"link\":"<<c.link<<",\"flags\":"<<c.flags<<",\"count\":"<<c.count<<",\"box\":[";
            for(int a=0;a<3;++a){if(a)std::cout<<',';std::cout<<c.request.queryMin[a];}
            for(int a=0;a<3;++a)std::cout<<','<<c.request.queryMax[a];std::cout<<"]}";
        }
        std::cout<<"],\"facets\":[";facet_probe::writeFacets(facets);std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
