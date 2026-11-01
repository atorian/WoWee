#include "collision/wmo_model_facets.hpp"
#include "collision/wmo_bsp_query.hpp"
#include "collision/liquid_facets.hpp"
#include <iterator>

namespace wowee::collision {
namespace {
struct BatchPool {
    uint32_t batches=0,indices=0,faces=0,status=0;
    bool allocate(uint32_t indexCount,uint32_t faceCount) {
        // AllocBatch (0x7c7710): strict limits, including 31 usable batches.
        if (batches+1>=32 || indices+indexCount>=0xc000 || faces+faceCount>=0x4000) {
            status|=1;
            return false;
        }
        ++batches;indices+=indexCount;faces+=faceCount;
        return true;
    }
    bool liquid(uint32_t reservation) {
        if (!allocate(0,0)) return false;
        // AllocIndices runs after the batch has already been consumed.
        if (indices+reservation>=0xc000) { status|=1;return false; }
        indices+=reservation;
        return true;
    }
};
}
WmoModelFacetResult appendWmoModelFacets(
    const WmoShape& model,const glm::vec3& low,const glm::vec3& high,
    uint32_t flags,const glm::mat4& transform,uint64_t owner,
    const WmoLiquidMaterials& materials,FacetNormalization normalization,
    std::vector<MovementFacet>& output) {
    WmoModelFacetResult result;
    BatchPool pool;
    const auto selected=[&](const WmoModelGroup& entry,uint32_t ignored) {
        const auto& b=*entry.bounds;
        return !(high.x<b.low.x || high.y<b.low.y || high.z<b.low.z ||
                 low.x>b.high.x || low.y>b.high.y || low.z>b.high.z) &&
               entry.shape->original->loadState==pipeline::WmoGroupLoadState::Ready &&
               !(entry.shape->groupFlags&ignored);
    };
    const auto liquid=[&](const WmoGroupShape& group,BatchPool& batches) {
        const auto& source=group.original->liquid;
        if (!source || !source->liquidType || !source->xTiles || !source->yTiles) return true;
        if (!source->valid) return false;
        uint32_t materialFlags=0;
        if ((flags&0x10000) && !(flags&0x20000)) {
            const auto found=materials.flags(source->liquidType);
            if (!found) return false;
            materialFlags=*found;
        }
        const WmoLiquidGrid grid{source->xVerts,source->yVerts,source->xTiles,source->yTiles,
            source->corner,source->vertices,source->tiles,source->liquidType,materialFlags};
        auto hit=queryWmoLiquidFacets(grid,low,high,flags,transform,owner,normalization);
        if (!hit) return false;
        if (!hit->facets.empty() && batches.liquid(hit->indexReservation)) {
            result.degenerateFaces+=hit->degenerateFaces;
            output.insert(output.end(),std::make_move_iterator(hit->facets.begin()),std::make_move_iterator(hit->facets.end()));
        }
        return true;
    };
    for (const auto& entry:model.originalGroups) {
        if (!entry.bounds || !entry.shape || !entry.shape->original) {
            result.status=MovementGeometryStatus::Failed;
            return result;
        }
    }
    if (flags&0xf0) for (const auto& entry:model.originalGroups) {
        if (!selected(entry,0x80)) continue;
        const auto& group=*entry.shape;
        const auto& source=*group.original;
        if (source.bsp) {
            const auto hit=queryWmoBsp(*source.bsp,group.vertices,group.indices,group.triMopyFlags,
                source.boundsMin,source.boundsMax,low,high,flags);
            if (hit.overflow) pool.status|=1;
            if (!hit.faces.empty() && pool.allocate(uint32_t(hit.faces.size()*3),uint32_t(hit.faces.size()))) {
                for (const auto face:hit.faces) {
                    const size_t at=size_t(face)*3;
                    const std::array<glm::vec3,3> vertices{group.vertices[group.indices[at]],
                        group.vertices[group.indices[at+1]],group.vertices[group.indices[at+2]]};
                    auto built=makeMovementFacet(vertices,transform,owner,normalization);
                    if (!built) { result.status=MovementGeometryStatus::Failed;return result; }
                    result.degenerateFaces+=built->degenerate;
                    output.push_back(built->facet);
                }
            }
        }
        if ((flags&0x30000) && !liquid(group,pool)) {
            result.status=MovementGeometryStatus::Failed;return result;
        }
    }
    result.originalStatusFlags=pool.status;
    if (flags&0x30000) for (const auto& entry:model.originalGroups) {
        if (!selected(entry,0x400080)) continue;
        // GetLiquidFacets resets the shared pool for every group. Its status
        // does not contribute to GetMapObjFacets' accumulated solid status.
        BatchPool liquidPool;
        if (!liquid(*entry.shape,liquidPool)) {
            result.status=MovementGeometryStatus::Failed;return result;
        }
    }
    return result;
}
} // namespace wowee::collision
