#include "collision/wmo_world_facets.hpp"
#include "collision/terrain_facets.hpp"
#include <cmath>

namespace wowee::collision {
namespace {
bool intersects(const WmoGroupBounds& a, const glm::vec3& low, const glm::vec3& high) {
    return !(high.x<a.low.x || high.y<a.low.y || high.z<a.low.z ||
             low.x>a.high.x || low.y>a.high.y || low.z>a.high.z);
}
WmoGroupBounds localQuery(const glm::vec3& center, const WmoGroupBounds& centered,
                         const glm::mat4& inverse) {
    WmoGroupBounds result{};
    for (int axis=0;axis<3;++axis) {
        // TransformAABox stores after each axis contribution; transforming
        // eight corners or accumulating in double changes query boundaries.
        for (int input=0;input<3;++input) {
            const double a=double(inverse[input][axis])*centered.low[input];
            const double b=double(inverse[input][axis])*centered.high[input];
            result.low[axis]=float(double(result.low[axis])+(a<b?a:b));
            result.high[axis]=float(double(result.high[axis])+(a<b?b:a));
        }
        const float localCenter=float(((double(inverse[2][axis])*center.z+
            double(inverse[1][axis])*center.y)+double(inverse[0][axis])*center.x)+inverse[3][axis]);
        result.low[axis]=float(double(result.low[axis])+localCenter);
        result.high[axis]=float(double(result.high[axis])+localCenter);
    }
    return result;
}
}
WmoModelFacetResult appendWorldWmoFacets(
    std::span<const WmoWorldPlacement> placements,
    std::span<const DestructibleCollisionProxy> proxies,
    const MovementFacetRequest& request,uint32_t flags,
    const WmoLiquidMaterials& materials,WmoLinkedFacets& linked,
    FacetNormalization normalization,std::vector<MovementFacet>& output) {
    WmoModelFacetResult result;
    glm::dvec3 preciseCenter;
    glm::vec3 center;
    WmoGroupBounds centered;
    for (int axis=0;axis<3;++axis) {
        preciseCenter[axis]=(double(request.queryMax[axis])+request.queryMin[axis])*.5;
        center[axis]=float(preciseCenter[axis]);
        // x87 retains the unrounded center for these subtractions and proxies.
        centered.low[axis]=float(double(request.queryMin[axis])-preciseCenter[axis]);
        centered.high[axis]=float(double(request.queryMax[axis])-preciseCenter[axis]);
    }
    for (const auto& proxy:proxies) {
        const glm::dvec3 extent=glm::dvec3(request.queryMax)-glm::dvec3(request.queryMin);
        const double radius=std::sqrt((extent.z*extent.z+extent.y*extent.y)+extent.x*extent.x)+proxy.radius;
        const glm::dvec3 delta=glm::dvec3(proxy.center)-preciseCenter;
        if ((delta.z*delta.z+delta.y*delta.y)+delta.x*delta.x<radius*radius) {
            result.status=MovementGeometryStatus::NotReady;
            return result;
        }
    }
    for (const auto& placement:placements) {
        if ((placement.flags&0x100) || !placement.model ||
            !intersects(placement.bounds,request.queryMin,request.queryMax)) continue;
        if (!placement.groupsReady || placement.groups.empty()) {
            if (!(flags&0x80000000u) || intersects(placement.bounds,request.bodyMin,request.bodyMax)) {
                result.status=MovementGeometryStatus::NotReady;
                return result;
            }
            appendMovementBoxFacets(placement.bounds.low,placement.bounds.high,output);
            continue;
        }
        const auto local=localQuery(center,centered,placement.inverse);
        const auto& model=*placement.model;
        if (intersects({model.boundingBoxMin,model.boundingBoxMax},local.low,local.high)) {
            const auto collected=appendWmoModelFacets(model,local.low,local.high,flags,
                placement.transform,placement.owner,materials,normalization,output);
            result.originalStatusFlags|=collected.originalStatusFlags;
            result.degenerateFaces+=collected.degenerateFaces;
            if (collected.status!=MovementGeometryStatus::Ready) {
                result.status=collected.status;
                return result;
            }
        }
        for (const auto& group:placement.groups) {
            if (!intersects(group.bounds,request.queryMin,request.queryMax)) continue;
            if (group.index>=model.originalGroups.size() || !model.originalGroups[group.index].shape ||
                !model.originalGroups[group.index].shape->original) {
                result.status=MovementGeometryStatus::Failed;
                return result;
            }
            const auto state=model.originalGroups[group.index].shape->original->loadState;
            if (state==pipeline::WmoGroupLoadState::Failed) {
                result.status=MovementGeometryStatus::Failed;
                return result;
            }
            if (state!=pipeline::WmoGroupLoadState::Ready) {
                if (!(flags&0x80000000u)) {
                    result.status=MovementGeometryStatus::NotReady;
                    return result;
                }
                // Unlike a whole missing building, this branch does not reject
                // intersection with the mover's body before adding the barrier.
                appendMovementBoxFacets(group.bounds.low,group.bounds.high,output);
                continue;
            }
            if (flags&0xf0000f) {
                result.status=linked.doodads(group.link,request,flags,output);
                if (result.status!=MovementGeometryStatus::Ready) return result;
            }
            if (flags&0xf00000) {
                result.status=linked.destructibles(group.link,request,flags,output);
                if (result.status!=MovementGeometryStatus::Ready) return result;
            }
        }
    }
    return result;
}
} // namespace wowee::collision
