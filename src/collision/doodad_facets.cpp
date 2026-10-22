#include "collision/doodad_facets.hpp"
#include "collision/m2_facets.hpp"
#include "collision/terrain_facets.hpp"
#include <iterator>

namespace wowee::collision {
void appendDoodadFacets(std::span<DoodadFacetPlacement* const> links,
    const MovementFacetRequest& request,uint32_t flags,uint32_t queryTag,
    std::vector<MovementFacet>& output) {
    if (!(flags&0xf0000f)) return;
    for (auto* placement:links) {
        auto& p=*placement;
        if ((p.flags&0x100) || p.queryTag==queryTag || !p.model) continue;
        if (p.owner?!(flags&0xf00000):!(flags&0xf)) continue;
        glm::vec3 low=p.boundsMin,high=p.boundsMax;
        if (!(p.flags&0x80)) {
            p.staticFlags|=0x10000;
            // CWorldMath::TransformAABox starts at translation and stores
            // each axis contribution separately, before testing the query.
            low=high=glm::vec3(p.transform[3]);
            for (int axis=0;axis<3;++axis) for (int input=0;input<3;++input) {
                const double a=double(p.transform[input][axis])*p.model->boundsMin[input];
                const double b=double(p.transform[input][axis])*p.model->boundsMax[input];
                low[axis]=float(double(low[axis])+(a<b?a:b));
                high[axis]=float(double(high[axis])+(a<b?b:a));
            }
        }
        if (!(request.queryMax.x<low.x || request.queryMax.y<low.y || request.queryMax.z<low.z ||
              request.queryMin.x>high.x || request.queryMin.y>high.y || request.queryMin.z>high.z)) {
            if (p.flags&0x80) {
                if (const auto* shape=p.model->shape) {
                    auto facets=queryM2Facets(shape->vertices,shape->indices,shape->faceNormals,
                        p.transform,request.queryMin,request.queryMax,p.owner);
                    output.insert(output.end(),std::make_move_iterator(facets.begin()),std::make_move_iterator(facets.end()));
                }
            } else appendMovementBoxFacets(low,high,output);
        }
        p.queryTag=queryTag;
    }
}
} // namespace wowee::collision
