#include "collision/wmo_shape.hpp"

#include "collision/geometry.hpp"
#include "collision/spatial_grid.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::collision {

namespace {

/// Which triangles a multi-cell gather has handed out already, per thread: a
/// stamp per triangle that is current when it equals `generation`. The group
/// used to keep a byte per triangle for this and clear it after each gather,
/// which was a write into a shared model from every querying thread.
struct VisitStamps {
    std::vector<uint32_t> stamp;
    uint32_t generation = 0;

    void begin(size_t triangles) {
        if (stamp.size() < triangles) stamp.resize(triangles, 0);
        if (++generation == 0) {
            std::fill(stamp.begin(), stamp.end(), 0);
            generation = 1;
        }
    }
    bool firstVisit(size_t index) {
        if (stamp[index] == generation) return false;
        stamp[index] = generation;
        return true;
    }
};
thread_local VisitStamps tl_visits;

}  // namespace

std::shared_ptr<const WmoGroupShape> WmoGroupShape::build(
        uint32_t groupFlags, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
        std::vector<glm::vec3> vertices, std::vector<uint16_t> indices,
        std::vector<glm::vec3> vertexColors, std::vector<uint8_t> mopyFlags) {
    auto shape = std::make_shared<WmoGroupShape>();
    WmoGroupShape& s = *shape;
    s.groupFlags = groupFlags;
    s.boundingBoxMin = boundsMin;
    s.boundingBoxMax = boundsMax;
    s.vertices = std::move(vertices);
    s.indices = std::move(indices);
    s.vertexColors = std::move(vertexColors);
    s.triMopyFlags = std::move(mopyFlags);

    // What this group offers to stand on, said once per group: a group that
    // has no triangles, or whose triangles are all detail, is a floor that
    // cannot be found, and neither shows up as anything but falling.
    const size_t tris = s.indices.size() / 3;
    {
        size_t hull = 0, renderedSolid = 0, detail = 0;
        for (uint8_t mopy : s.triMopyFlags) {
            if (mopy & 0x08) ++hull;
            if ((mopy & 0x20) && !(mopy & 0x04)) ++renderedSolid;
            if (mopy & 0x04) ++detail;
        }
        s.noBlockingTriangles = (tris > 0 && hull == 0 && renderedSolid == 0);
        if (tris == 0 || s.noBlockingTriangles) {
            LOG_WARNING("WMO group offers no floor: verts=", s.vertices.size(),
                        " tris=", tris,
                        " hull(0x08)=", hull,
                        " renderedSolid(0x20 not 0x04)=", renderedSolid,
                        " detail(0x04)=", detail,
                        " - nothing here blocks, so anything standing on this "
                        "group falls through it");
        }
    }

    if (s.vertices.empty() || s.indices.size() < 3) return shape;

    s.gridOrigin = glm::vec2(s.boundingBoxMin.x, s.boundingBoxMin.y);
    const float extentX = s.boundingBoxMax.x - s.boundingBoxMin.x;
    const float extentY = s.boundingBoxMax.y - s.boundingBoxMin.y;
    // Capped: a huge group would otherwise take more memory in empty cells
    // than in triangles.
    s.gridCellsX = std::min(64, std::max(1, static_cast<int>(std::ceil(extentX / kCellSize))));
    s.gridCellsY = std::min(64, std::max(1, static_cast<int>(std::ceil(extentY / kCellSize))));

    const size_t totalCells = static_cast<size_t>(s.gridCellsX) * static_cast<size_t>(s.gridCellsY);
    s.cellTriangles.resize(totalCells);
    s.cellFloorTriangles.resize(totalCells);
    s.cellWallTriangles.resize(totalCells);
    s.triBounds.resize(tris);
    s.triNormals.resize(tris);

    const float invCellW = s.gridCellsX / std::max(0.01f, extentX);
    const float invCellH = s.gridCellsY / std::max(0.01f, extentY);

    for (size_t i = 0; i + 2 < s.indices.size(); i += 3) {
        const glm::vec3& v0 = s.vertices[s.indices[i]];
        const glm::vec3& v1 = s.vertices[s.indices[i + 1]];
        const glm::vec3& v2 = s.vertices[s.indices[i + 2]];

        const float triMinX = std::min({v0.x, v1.x, v2.x});
        const float triMinY = std::min({v0.y, v1.y, v2.y});
        const float triMaxX = std::max({v0.x, v1.x, v2.x});
        const float triMaxY = std::max({v0.y, v1.y, v2.y});
        s.triBounds[i / 3] = {.minZ = std::min({v0.z, v1.z, v2.z}),
                              .maxZ = std::max({v0.z, v1.z, v2.z})};

        glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
        const float normalLen = glm::length(normal);
        normal = normalLen > 0.001f ? normal / normalLen : glm::vec3(0.0f, 0.0f, 1.0f);
        s.triNormals[i / 3] = normal;

        const float absNz = std::abs(normal.z);
        const bool isFloor = (absNz >= kWallMaxAbsNormalZ);
        const bool isWall = (absNz < kWallMaxAbsNormalZ);

        const int cellMinX = std::max(0, static_cast<int>((triMinX - s.gridOrigin.x) * invCellW));
        const int cellMinY = std::max(0, static_cast<int>((triMinY - s.gridOrigin.y) * invCellH));
        const int cellMaxX = std::min(s.gridCellsX - 1, static_cast<int>((triMaxX - s.gridOrigin.x) * invCellW));
        const int cellMaxY = std::min(s.gridCellsY - 1, static_cast<int>((triMaxY - s.gridOrigin.y) * invCellH));

        const uint32_t triStart = static_cast<uint32_t>(i);
        for (int cy = cellMinY; cy <= cellMaxY; ++cy) {
            for (int cx = cellMinX; cx <= cellMaxX; ++cx) {
                const int cellIdx = cy * s.gridCellsX + cx;
                s.cellTriangles[cellIdx].push_back(triStart);
                if (isFloor) s.cellFloorTriangles[cellIdx].push_back(triStart);
                if (isWall) s.cellWallTriangles[cellIdx].push_back(triStart);
            }
        }
    }
    return shape;
}

/// The triangles of one cell array that a query box reaches.
///
/// The dedup matters because a triangle spanning several cells is filed under
/// each of them, and a caller that tests it twice counts two hits: a raycast
/// then reports an even number of crossings where there was one surface,
/// which is how a solid wall reads as empty air. One cell cannot hand out the
/// same triangle twice, so the commonest case skips it.
void WmoGroupShape::gatherCellTriangles(const std::vector<std::vector<uint32_t>>& cells,
                                        float minX, float minY, float maxX, float maxY,
                                        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0 || cells.empty()) return;

    const auto range = cellRangeCovering(
        gridCellsX, gridCellsY,
        boundingBoxMax.x - boundingBoxMin.x, boundingBoxMax.y - boundingBoxMin.y,
        gridOrigin, minX, minY, maxX, maxY);
    if (!range) return;

    // About eight triangles a cell, which is what the meshes here average.
    out.reserve(range->count() * 8);

    const bool multiCell = (range->minX != range->maxX || range->minY != range->maxY);
    if (multiCell) {
        auto& visits = tl_visits;
        visits.begin(indices.size() / 3);
        for (int cy = range->minY; cy <= range->maxY; ++cy) {
            for (int cx = range->minX; cx <= range->maxX; ++cx) {
                for (uint32_t tri : cells[cy * gridCellsX + cx]) {
                    if (visits.firstVisit(tri / 3)) out.push_back(tri);
                }
            }
        }
    } else {
        const auto& cell = cells[range->minY * gridCellsX + range->minX];
        out.insert(out.end(), cell.begin(), cell.end());
    }
}

void WmoGroupShape::trianglesInRange(float minX, float minY, float maxX, float maxY,
                                     std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellTriangles, minX, minY, maxX, maxY, out);
}

void WmoGroupShape::floorTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                          std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellFloorTriangles, minX, minY, maxX, maxY, out);
}

void WmoGroupShape::wallTrianglesInRange(float minX, float minY, float maxX, float maxY,
                                         std::vector<uint32_t>& out) const {
    gatherCellTriangles(cellWallTriangles, minX, minY, maxX, maxY, out);
}

bool WmoGroupShape::trianglesAlongRay(const glm::vec3& localOrigin, const glm::vec3& localDir,
                                      std::vector<uint32_t>& out) const {
    float tEnter = 0.0f, tExit = 0.0f;
    if (!rayAABBRange(localOrigin, localDir, boundingBoxMin, boundingBoxMax, tEnter, tExit)) {
        return false;
    }
    const glm::vec3 enter = localOrigin + localDir * tEnter;
    const glm::vec3 exit = localOrigin + localDir * tExit;
    trianglesInRange(std::min(enter.x, exit.x) - 1.0f, std::min(enter.y, exit.y) - 1.0f,
                     std::max(enter.x, exit.x) + 1.0f, std::max(enter.y, exit.y) + 1.0f, out);
    return true;
}

uint64_t WmoGroupShape::bytes() const {
    uint64_t total = sizeof(WmoGroupShape) +
                     vertices.capacity() * sizeof(glm::vec3) +
                     indices.capacity() * sizeof(uint16_t) +
                     vertexColors.capacity() * sizeof(glm::vec3) +
                     triBounds.capacity() * sizeof(TriBounds) +
                     triNormals.capacity() * sizeof(glm::vec3) +
                     triMopyFlags.capacity();
    for (const auto* cells : {&cellTriangles, &cellFloorTriangles, &cellWallTriangles}) {
        total += cells->capacity() * sizeof(std::vector<uint32_t>);
        for (const auto& cell : *cells) total += cell.capacity() * sizeof(uint32_t);
    }
    return total;
}

uint64_t WmoShape::bytes() const {
    uint64_t total = sizeof(WmoShape) + groups.capacity() * sizeof(groups[0]);
    for (const auto& group : groups) {
        if (group) total += group->bytes();
    }
    return total;
}

}  // namespace wowee::collision
