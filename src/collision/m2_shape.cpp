#include "collision/m2_shape.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wowee::collision {

std::shared_ptr<const M2Shape> M2Shape::build(std::string name, std::vector<glm::vec3> vertices,
                                              std::vector<uint16_t> indices, std::vector<glm::vec3> faceNormals) {
    if (faceNormals.size() > indices.size() / 3) {
        LOG_ERROR("M2 '", name, "' has more collision face normals than indexed triangles - collision shape unavailable");
        return nullptr;
    }
    for (size_t i = 0; i < faceNormals.size() * 3; ++i) {
        if (indices[i] >= vertices.size()) {
            LOG_ERROR("M2 '", name, "' collision face ", i / 3, " references vertex ", indices[i],
                      " of ", vertices.size(), " - collision shape unavailable");
            return nullptr;
        }
    }
    if (indices.size() < 3 || vertices.empty()) return nullptr;

    auto shape = std::make_shared<M2Shape>();
    M2Shape& s = *shape;
    s.name = std::move(name);
    s.vertices = std::move(vertices);
    s.indices = std::move(indices);
    s.faceNormals = std::move(faceNormals);
    s.triCount = static_cast<uint32_t>(s.indices.size() / 3);

    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(-std::numeric_limits<float>::max());
    for (const auto& v : s.vertices) {
        bmin = glm::min(bmin, v);
        bmax = glm::max(bmax, v);
    }
    s.boundMin = bmin;
    s.boundMax = bmax;

    s.gridOrigin = glm::vec2(bmin.x, bmin.y);
    s.gridCellsX = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.x - bmin.x) / kCellSize))));
    s.gridCellsY = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.y - bmin.y) / kCellSize))));

    const size_t cells = static_cast<size_t>(s.gridCellsX) * static_cast<size_t>(s.gridCellsY);
    s.cellTris.resize(cells);
    s.cellWallTris.resize(cells);
    s.triBounds.resize(s.triCount);

    for (uint32_t ti = 0; ti < s.triCount; ti++) {
        const uint16_t i0 = s.indices[ti * 3];
        const uint16_t i1 = s.indices[ti * 3 + 1];
        const uint16_t i2 = s.indices[ti * 3 + 2];
        if (i0 >= s.vertices.size() || i1 >= s.vertices.size() || i2 >= s.vertices.size()) continue;

        const auto& v0 = s.vertices[i0];
        const auto& v1 = s.vertices[i1];
        const auto& v2 = s.vertices[i2];

        s.triBounds[ti].minZ = std::min({v0.z, v1.z, v2.z});
        s.triBounds[ti].maxZ = std::max({v0.z, v1.z, v2.z});

        const glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
        const float normalLen = glm::length(normal);
        const float absNz = (normalLen > 0.001f) ? std::abs(normal.z / normalLen) : 0.0f;
        const bool isWall = (absNz < 0.65f);

        const float triMinX = std::min({v0.x, v1.x, v2.x});
        const float triMaxX = std::max({v0.x, v1.x, v2.x});
        const float triMinY = std::min({v0.y, v1.y, v2.y});
        const float triMaxY = std::max({v0.y, v1.y, v2.y});

        const int cxMin = std::clamp(static_cast<int>((triMinX - s.gridOrigin.x) / kCellSize), 0, s.gridCellsX - 1);
        const int cxMax = std::clamp(static_cast<int>((triMaxX - s.gridOrigin.x) / kCellSize), 0, s.gridCellsX - 1);
        const int cyMin = std::clamp(static_cast<int>((triMinY - s.gridOrigin.y) / kCellSize), 0, s.gridCellsY - 1);
        const int cyMax = std::clamp(static_cast<int>((triMaxY - s.gridOrigin.y) / kCellSize), 0, s.gridCellsY - 1);

        for (int cy = cyMin; cy <= cyMax; cy++) {
            for (int cx = cxMin; cx <= cxMax; cx++) {
                const int ci = cy * s.gridCellsX + cx;
                s.cellTris[ci].push_back(ti);
                if (isWall) s.cellWallTris[ci].push_back(ti);
            }
        }
    }
    return shape;
}

/// The triangles of one cell array that a query box reaches, deduplicated.
///
/// A triangle spanning several cells is filed under each, so the sort and
/// unique are not tidiness - a caller that tests the same triangle twice
/// counts two hits, and a raycast then reports an even number of crossings
/// where there was one surface.
void M2Shape::gatherTrisInRange(const std::vector<std::vector<uint32_t>>& cells,
                                float minX, float minY, float maxX, float maxY,
                                std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0) return;

    const int cxMin = std::clamp(static_cast<int>((minX - gridOrigin.x) / kCellSize), 0, gridCellsX - 1);
    const int cxMax = std::clamp(static_cast<int>((maxX - gridOrigin.x) / kCellSize), 0, gridCellsX - 1);
    const int cyMin = std::clamp(static_cast<int>((minY - gridOrigin.y) / kCellSize), 0, gridCellsY - 1);
    const int cyMax = std::clamp(static_cast<int>((maxY - gridOrigin.y) / kCellSize), 0, gridCellsY - 1);

    const size_t cellCount = static_cast<size_t>(cxMax - cxMin + 1) *
                             static_cast<size_t>(cyMax - cyMin + 1);
    out.reserve(cellCount * 8);
    for (int cy = cyMin; cy <= cyMax; cy++) {
        for (int cx = cxMin; cx <= cxMax; cx++) {
            const auto& cell = cells[cy * gridCellsX + cx];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void M2Shape::trisInRange(float minX, float minY, float maxX, float maxY,
                               std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellTris, minX, minY, maxX, maxY, out);
}

void M2Shape::wallTrisInRange(float minX, float minY, float maxX, float maxY,
                              std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellWallTris, minX, minY, maxX, maxY, out);
}

uint64_t M2Shape::bytes() const {
    uint64_t total = sizeof(M2Shape) + name.capacity() +
                     vertices.capacity() * sizeof(glm::vec3) +
                     indices.capacity() * sizeof(uint16_t) +
                     faceNormals.capacity() * sizeof(glm::vec3) +
                     triBounds.capacity() * sizeof(TriBounds);
    for (const auto* cells : {&cellTris, &cellWallTris}) {
        total += cells->capacity() * sizeof(std::vector<uint32_t>);
        for (const auto& cell : *cells) total += cell.capacity() * sizeof(uint32_t);
    }
    return total;
}

}  // namespace wowee::collision
