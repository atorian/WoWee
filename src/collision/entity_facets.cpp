#include "collision/entity_facets.hpp"
#include "collision/m2_facets.hpp"
#include <iterator>

namespace wowee::collision {
void appendEntityFacets(std::span<EntityFacetPlacement* const> links,
    EntityFacetProvider* provider,const MovementFacetRequest& request,
    uint32_t flags,uint32_t queryTag,std::vector<MovementFacet>& output) {
    if (!provider || !(flags&0xf00000)) return;
    for (auto* placement:links) {
        auto& p=*placement;
        if (!(p.staticFlags&2)) break;
        if (p.queryTag==queryTag || (p.staticFlags&4)) continue;
        uint64_t owner=p.owner;
        const auto data=provider->query(owner,p.parameter,flags);
        if (data && data->shape &&
            !(request.queryMax.x<data->boundsMin.x || request.queryMax.y<data->boundsMin.y || request.queryMax.z<data->boundsMin.z ||
              request.queryMin.x>data->boundsMax.x || request.queryMin.y>data->boundsMax.y || request.queryMin.z>data->boundsMax.z)) {
            const auto& shape=*data->shape;
            auto facets=queryM2Facets(shape.vertices,shape.indices,shape.faceNormals,
                data->transform,request.queryMin,request.queryMax,owner);
            output.insert(output.end(),std::make_move_iterator(facets.begin()),std::make_move_iterator(facets.end()));
        }
        p.queryTag=queryTag;
    }
}
} // namespace wowee::collision
