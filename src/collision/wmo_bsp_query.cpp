#include "collision/wmo_bsp_query.hpp"
#include <array>

namespace wowee::collision {
namespace {
uint16_t ignoredFaces(uint32_t flags) {
    // CMapObj::CreateWmoIgnoreFlags, 0x7ae140. The visitation bit is added by
    // GetTris, not read as a substitute for collision membership in the BSP.
    uint32_t result = 0xee;
    if (flags & 0x80) return 0x2ca;
    if (flags & 0x10) result = 0xc6;
    if (flags & 0x20) result &= 0xdb;
    if (!(flags & 0x40)) result &= ~2u;
    if (!(flags & 0x4000)) result &= ~0x40u;
    if (flags & 0x1000000) result = flags & 0x40000000 ? 0xfffffcdb : result | 0x100;
    return uint16_t(result);
}
}

WmoBspQueryResult queryWmoBsp(const pipeline::WmoBspData& bsp,
    std::span<const glm::vec3> vertices, std::span<const uint16_t> indices,
    std::span<const uint8_t> mopy, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
    const glm::vec3& queryMin, const glm::vec3& queryMax,
    uint32_t queryFlags, bool digestCacheEnabled) {
    WmoBspQueryResult result;
    const uint16_t ignore = ignoredFaces(queryFlags) | 0x80;
    std::vector<uint8_t> visited(indices.size()/3, false);
    struct Visit { uint16_t node; glm::vec3 low, high, boundsLow, boundsHigh; };
    std::vector<Visit> stack{{0, queryMin, queryMax, boundsMin, boundsMax}};
    while (!stack.empty()) {
        const Visit visit = stack.back();
        stack.pop_back();
        const auto& node = bsp.nodes[visit.node];
        if (node.flags & 4) {
            const bool digest = digestCacheEnabled && bsp.digestEligible[visit.node];
            for (size_t i=node.firstFace; i<size_t(node.firstFace)+node.faceCount; ++i) {
                const uint16_t face = bsp.faces[i];
                if (visited[face] || (mopy[face] & ignore)) continue;
                if (result.tested.size() >= 8192) { result.overflow = true; break; }
                visited[face] = true;
                result.tested.push_back(face);
                bool outside = false;
                for (int axis=0; axis<3; ++axis) {
                    const float a=vertices[indices[size_t(face)*3]][axis];
                    const float b=vertices[indices[size_t(face)*3+1]][axis];
                    const float c=vertices[indices[size_t(face)*3+2]][axis];
                    // The cached original uses <=/>= outcodes. Its uncached
                    // TriangleOutsideAaBox uses strict inequalities. These
                    // paths are observably different at an exact boundary.
                    if (digest) {
                        outside |= (a<=queryMin[axis] && b<=queryMin[axis] && c<=queryMin[axis]) ||
                                   (a>=queryMax[axis] && b>=queryMax[axis] && c>=queryMax[axis]);
                    } else {
                        outside |= (a<queryMin[axis] && b<queryMin[axis] && c<queryMin[axis]) ||
                                   (a>queryMax[axis] && b>queryMax[axis] && c>queryMax[axis]);
                    }
                }
                if (!outside) result.faces.push_back(face);
            }
            continue;
        }
        const int axis = node.flags & 3;
        if (visit.boundsLow[axis] > visit.high[axis] || visit.boundsHigh[axis] < visit.low[axis]) continue;
        Visit positive = visit, negative = visit;
        positive.node = node.positiveChild;
        negative.node = node.negativeChild;
        positive.boundsLow[axis] = node.planeDistance;
        negative.boundsHigh[axis] = node.planeDistance;
        if (node.planeDistance < visit.low[axis]) {
            if (positive.node != 0xffff) stack.push_back(positive);
        } else if (node.planeDistance > visit.high[axis]) {
            if (negative.node != 0xffff) stack.push_back(negative);
        } else {
            positive.low[axis] = node.planeDistance;
            negative.high[axis] = node.planeDistance;
            // LIFO: the original visits positive, then negative, at a split.
            if (negative.node != 0xffff) stack.push_back(negative);
            if (positive.node != 0xffff) stack.push_back(positive);
        }
    }
    return result;
}
} // namespace wowee::collision
