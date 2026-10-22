#pragma once

#include "rendering/frustum.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace wowee::rendering {

template<class Geometry>
struct TerrainDepthChunk {
    Geometry geometry;
    glm::vec3 centre{0};
    float radius = 0;
};

template<class Geometry>
struct TerrainDepthTile {
    glm::vec3 centre{0};
    float radius = 0;
    std::vector<TerrainDepthChunk<Geometry>> chunks;
};

/// The producer invalidates this when a tile gains geometry. Old frames keep
/// their immutable copy when streaming replaces or removes the live tile.
/// Buffer lifetime is still covered by the frame's snapshot resource lease.
template<class Geometry>
class TerrainDepthCache {
public:
    using Tile = TerrainDepthTile<Geometry>;
    void invalidate() { snapshot_.reset(); }

    template<class ReadChunk>
    std::shared_ptr<const Tile> extract(std::span<const uint32_t> indices, ReadChunk&& read) {
        if (snapshot_) return snapshot_;
        auto tile = std::make_shared<Tile>();
        tile->chunks.reserve(indices.size());
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (const uint32_t index : indices) {
            auto chunk = read(index);
            lo = glm::min(lo, chunk.centre);
            hi = glm::max(hi, chunk.centre);
            tile->chunks.push_back(std::move(chunk));
        }
        if (!tile->chunks.empty()) {
            tile->centre = (lo + hi) * 0.5f;
            for (const auto& chunk : tile->chunks)
                tile->radius = std::max(tile->radius, glm::length(chunk.centre - tile->centre) + chunk.radius);
            // Round outward so the broad bound cannot lose a tangent child.
            tile->radius = std::nextafter(tile->radius, std::numeric_limits<float>::infinity());
        }
        snapshot_ = std::move(tile);
        return snapshot_;
    }

private:
    std::shared_ptr<const Tile> snapshot_;
};

struct TerrainDepthVisit {
    size_t tilesTested = 0, chunksTested = 0, drawn = 0;
};

/// All depth consumers use the same conservative bounds: sun, lightning,
/// shelter and sea bed. This reads only immutable frame data, never the scene.
template<class Geometry, class Draw>
TerrainDepthVisit visitTerrainDepth(
        const std::vector<std::shared_ptr<const TerrainDepthTile<Geometry>>>& tiles,
        const glm::mat4& matrix, const glm::vec3& centre, float radius, Draw&& draw) {
    const glm::vec4 centreLs = matrix * glm::vec4(centre, 1);
    const float scaleZ = glm::length(glm::vec3(matrix[0][2], matrix[1][2], matrix[2][2]));
    const bool perspective = glm::length(glm::vec3(matrix[0][3], matrix[1][3], matrix[2][3])) > 1e-6f;
    Frustum frustum;
    frustum.extractFromMatrix(matrix);
    const auto mayCast = [&](const glm::vec3& point, float bound) {
        if (!frustum.intersectsSphere(point, bound)) return false;
        if (perspective) return true;
        const glm::vec4 ls = matrix * glm::vec4(point, 1);
        return ls.z <= centreLs.z + (radius + bound) * scaleZ;
    };
    TerrainDepthVisit result;
    for (const auto& tile : tiles) {
        ++result.tilesTested;
        if (!mayCast(tile->centre, tile->radius)) continue;
        for (const auto& chunk : tile->chunks) {
            ++result.chunksTested;
            if (!mayCast(chunk.centre, chunk.radius)) continue;
            draw(chunk.geometry);
            ++result.drawn;
        }
    }
    return result;
}

} // namespace wowee::rendering
