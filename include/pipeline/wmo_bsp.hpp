#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wowee::pipeline {

// MOBN records and MOBR face references, in authored order. These are collision
// topology, independent of render batches and authored shading normals.
struct WmoBspNode {
    uint16_t flags = 4;
    uint16_t negativeChild = 0xffff;
    uint16_t positiveChild = 0xffff;
    uint16_t faceCount = 0;
    uint32_t firstFace = 0;
    float planeDistance = 0;
};
struct WmoBspData {
    std::vector<WmoBspNode> nodes;
    std::vector<uint16_t> faces;
    // Original digest path accepts <=300 faces with <=450 distinct vertices.
    // Its boundary comparisons differ from the uncached path. Compute this
    // property on the loading worker, never from render visibility or LOD.
    std::vector<uint8_t> digestEligible;

    // Malformed topology fails before publication. The loader supplies its
    // file/group context when reporting error; this pure builder does no I/O.
    [[nodiscard]] static std::shared_ptr<const WmoBspData> build(
        std::vector<WmoBspNode> nodes, std::vector<uint16_t> faces,
        std::span<const uint16_t> triangleIndices, size_t vertexCount,
        std::string& error);
};
} // namespace wowee::pipeline
