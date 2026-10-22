#include "collision/collision_world.hpp"

#include "collision/wmo_group_containment.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "pipeline/terrain_mesh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>

namespace wowee::collision {

namespace {

// Scratch for the queries, per thread: the camera runs its building and doodad
// floor queries on two frame workers at once.
thread_local std::unordered_set<uint32_t> tl_seenIds;
thread_local std::vector<uint32_t> tl_triScratch;

/// One chunk of terrain, 33.3 units across: a tile is 16 of them each way.
constexpr float kChunkSize = core::coords::TILE_SIZE / 16.0f;

uint64_t tileKey(int x, int y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(y));
}

/// Adds a query's time to its kind's total when it goes out of scope.
class QueryTimer {
public:
    QueryTimer(std::atomic<double>& ms, std::atomic<uint32_t>& calls)
        : ms_(ms), calls_(calls), start_(std::chrono::steady_clock::now()) {}
    ~QueryTimer() {
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_).count();
        double current = ms_.load(std::memory_order_relaxed);
        while (!ms_.compare_exchange_weak(current, current + elapsed, std::memory_order_relaxed)) {}
        calls_.fetch_add(1, std::memory_order_relaxed);
    }
    QueryTimer(const QueryTimer&) = delete;
    QueryTimer& operator=(const QueryTimer&) = delete;

private:
    std::atomic<double>& ms_;
    std::atomic<uint32_t>& calls_;
    std::chrono::steady_clock::time_point start_;
};

/// Whether a point is inside an instance's world box, with the Z window
/// widened by the caller's margins.
bool withinBounds(const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                  float x, float y, float z, float zMarginDown = 0.0f, float zMarginUp = 0.0f) {
    return x >= boundsMin.x && x <= boundsMax.x &&
           y >= boundsMin.y && y <= boundsMax.y &&
           z >= boundsMin.z - zMarginDown && z <= boundsMax.z + zMarginUp;
}

}  // namespace

CollisionWorld::CollisionWorld() = default;
CollisionWorld::~CollisionWorld() = default;

// ---- Doodads ---------------------------------------------------------------

void CollisionWorld::boundM2(M2Placement& p) const {
    transformAABB(p.transform, p.shape->boundMin, p.shape->boundMax, p.boundsMin, p.boundsMax);
}

void CollisionWorld::placeM2(uint32_t instanceId, std::shared_ptr<const M2Shape> shape,
                             const glm::mat4& transform, float scale) {
    if (!shape) return;
    removeM2(instanceId);
    M2Placement p;
    p.id = instanceId;
    p.shape = std::move(shape);
    p.transform = transform;
    p.inverse = glm::inverse(transform);
    p.scale = scale;
    boundM2(p);
    insertBounds(m2Grid_, p.boundsMin, p.boundsMax, instanceId);
    m2_.emplace(instanceId, std::move(p));
}

void CollisionWorld::moveM2(uint32_t instanceId, const glm::mat4& transform, float scale) {
    const auto it = m2_.find(instanceId);
    if (it == m2_.end()) return;
    M2Placement& p = it->second;
    const glm::vec3 oldMin = p.boundsMin, oldMax = p.boundsMax;
    p.transform = transform;
    p.inverse = glm::inverse(transform);
    p.scale = scale;
    boundM2(p);
    // A hidden instance is not in the grid; it is filed where it then is
    // once shown.
    if (!p.hidden) refileBounds(m2Grid_, oldMin, oldMax, p.boundsMin, p.boundsMax, instanceId);
}

void CollisionWorld::removeM2(uint32_t instanceId) {
    const auto it = m2_.find(instanceId);
    if (it == m2_.end()) return;
    if (!it->second.hidden) {
        eraseBounds(m2Grid_, it->second.boundsMin, it->second.boundsMax, instanceId);
    }
    m2_.erase(it);
}

void CollisionWorld::setM2Hidden(uint32_t instanceId, bool hidden) {
    const auto it = m2_.find(instanceId);
    if (it == m2_.end() || it->second.hidden == hidden) return;
    M2Placement& p = it->second;
    p.hidden = hidden;
    if (hidden) eraseBounds(m2Grid_, p.boundsMin, p.boundsMax, instanceId);
    else insertBounds(m2Grid_, p.boundsMin, p.boundsMax, instanceId);
}

void CollisionWorld::setM2SkipCollision(uint32_t instanceId, bool skip) {
    const auto it = m2_.find(instanceId);
    if (it != m2_.end()) it->second.skipCollision = skip;
}

void CollisionWorld::setM2SkipWalls(uint32_t instanceId, bool skip) {
    const auto it = m2_.find(instanceId);
    if (it != m2_.end()) it->second.skipWalls = skip;
}

void CollisionWorld::clearM2() {
    m2_.clear();
    m2Grid_.clear();
}

void CollisionWorld::gatherM2(const glm::vec3& queryMin, const glm::vec3& queryMax,
                              std::vector<const M2Placement*>& out) const {
    out.clear();
    gatherIds(m2Grid_, queryMin, queryMax, tl_seenIds, [&](uint32_t id) {
        const auto it = m2_.find(id);
        if (it != m2_.end()) out.push_back(&it->second);
    });
}

// ---- Buildings ---------------------------------------------------------------

void CollisionWorld::boundWmo(WmoPlacement& p) const {
    const WmoShape& shape = *p.shape;
    transformAABB(p.transform, shape.boundingBoxMin, shape.boundingBoxMax, p.boundsMin, p.boundsMax);
    p.groupBounds.clear();
    p.groupBounds.reserve(shape.groups.size());
    for (const auto& group : shape.groups) {
        glm::vec3 gMin, gMax;
        transformAABB(p.transform, group->boundingBoxMin, group->boundingBoxMax, gMin, gMax);
        p.groupBounds.emplace_back(gMin - glm::vec3(0.5f), gMax + glm::vec3(0.5f));
    }
}

void CollisionWorld::placeWmo(uint32_t instanceId, std::shared_ptr<const WmoShape> shape,
                              const glm::mat4& transform) {
    if (!shape) return;
    removeWmo(instanceId);
    WmoPlacement p;
    p.id = instanceId;
    p.shape = std::move(shape);
    p.transform = transform;
    p.inverse = glm::inverse(transform);
    boundWmo(p);
    insertBounds(wmoGrid_, p.boundsMin, p.boundsMax, instanceId);
    wmo_.emplace(instanceId, std::move(p));
    ++wmoRevision_;
}

void CollisionWorld::moveWmo(uint32_t instanceId, const glm::mat4& transform) {
    const auto it = wmo_.find(instanceId);
    if (it == wmo_.end()) return;
    WmoPlacement& p = it->second;
    const glm::vec3 oldMin = p.boundsMin, oldMax = p.boundsMax;
    p.transform = transform;
    p.inverse = glm::inverse(transform);
    boundWmo(p);
    if (!p.hidden) refileBounds(wmoGrid_, oldMin, oldMax, p.boundsMin, p.boundsMax, instanceId);
}

void CollisionWorld::removeWmo(uint32_t instanceId) {
    const auto it = wmo_.find(instanceId);
    if (it == wmo_.end()) return;
    if (!it->second.hidden) {
        eraseBounds(wmoGrid_, it->second.boundsMin, it->second.boundsMax, instanceId);
    }
    wmo_.erase(it);
    ++wmoRevision_;
}

void CollisionWorld::setWmoHidden(uint32_t instanceId, bool hidden) {
    const auto it = wmo_.find(instanceId);
    if (it == wmo_.end() || it->second.hidden == hidden) return;
    WmoPlacement& p = it->second;
    p.hidden = hidden;
    if (hidden) eraseBounds(wmoGrid_, p.boundsMin, p.boundsMax, instanceId);
    else insertBounds(wmoGrid_, p.boundsMin, p.boundsMax, instanceId);
    ++wmoRevision_;
}

void CollisionWorld::setWmoTransport(uint32_t instanceId, bool isTransport) {
    const auto it = wmo_.find(instanceId);
    if (it != wmo_.end()) it->second.isTransport = isTransport;
}

void CollisionWorld::clearWmo() {
    wmo_.clear();
    wmoGrid_.clear();
    ++wmoRevision_;
}

void CollisionWorld::gatherWmo(const glm::vec3& queryMin, const glm::vec3& queryMax,
                               std::vector<const WmoPlacement*>& out) const {
    out.clear();
    gatherIds(wmoGrid_, queryMin, queryMax, tl_seenIds, [&](uint32_t id) {
        const auto it = wmo_.find(id);
        if (it != wmo_.end()) out.push_back(&it->second);
    });
}

// ---- Terrain -----------------------------------------------------------------

void CollisionWorld::addTerrainTile(std::shared_ptr<const TerrainTileHeights> tile) {
    if (!tile) return;
    const uint64_t key = tileKey(tile->tileX, tile->tileY);
    terrain_[key] = std::move(tile);
}

void CollisionWorld::removeTerrainTile(int tileX, int tileY) {
    terrain_.erase(tileKey(tileX, tileY));
}

void CollisionWorld::clearTerrain() { terrain_.clear(); }

void CollisionWorld::clear() {
    clearM2();
    clearWmo();
    clearTerrain();
}

/// The chunk under a point, and where in it the point falls.
///
/// The tile the point is in first, guessing the chunk from the tile's corner
/// and probing round the guess; then every chunk of that tile; then every
/// loaded tile, for points on a seam the guess rounds the wrong way.
const TerrainTileHeights::Chunk* CollisionWorld::findChunkAt(float x, float y, float& fracX,
                                                             float& fracY) const {
    const float unitSize = kChunkSize / 8.0f;

    auto inChunk = [&](const TerrainTileHeights& tile, int cx, int cy)
            -> const TerrainTileHeights::Chunk* {
        if (cx < 0 || cx >= 16 || cy < 0 || cy >= 16) return nullptr;
        const auto& chunk = tile.chunks[cy * 16 + cx];
        if (!chunk.hasHeights()) return nullptr;
        if (!pipeline::TerrainMeshGenerator::chunkFractionsAt(chunk.position, x, y, unitSize,
                                                              fracX, fracY)) {
            return nullptr;
        }
        return &chunk;
    };

    auto inTile = [&](const TerrainTileHeights& tile) -> const TerrainTileHeights::Chunk* {
        // The tile's render-space corner: its X index counts along render Y.
        const float maxX = (32 - tile.tileY) * core::coords::TILE_SIZE;
        const float maxY = (32 - tile.tileX) * core::coords::TILE_SIZE;
        const int guessCy = glm::clamp(static_cast<int>(std::floor((maxX - x) / kChunkSize)), 0, 15);
        const int guessCx = glm::clamp(static_cast<int>(std::floor((maxY - y) / kChunkSize)), 0, 15);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (auto* c = inChunk(tile, guessCx + dx, guessCy + dy)) return c;
            }
        }
        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++) {
                if (auto* c = inChunk(tile, cx, cy)) return c;
            }
        }
        return nullptr;
    };

    const auto [tileX, tileY] = core::coords::worldToTile(x, y);
    const uint64_t key = tileKey(tileX, tileY);
    if (const auto it = terrain_.find(key); it != terrain_.end()) {
        if (auto* c = inTile(*it->second)) return c;
    }
    for (const auto& [otherKey, tile] : terrain_) {
        if (otherKey == key) continue;
        if (auto* c = inTile(*tile)) return c;
    }
    return nullptr;
}

std::optional<float> CollisionWorld::terrainHeight(float x, float y) const {
    float fracX = 0.0f, fracY = 0.0f;
    const auto* chunk = findChunkAt(x, y, fracX, fracY);
    if (!chunk) return std::nullopt;
    // The one sampler, shared with the mesh builder and the clutter
    // scatterer: the mesh fans four triangles from each quad's centre vertex,
    // and a bilinear blend of the corners put the floor below the drawn
    // ground wherever the artist moved that centre.
    const glm::vec3 surface = pipeline::TerrainMeshGenerator::chunkSurfacePoint(
        chunk->position, chunk->heightMap, fracX, fracY, kChunkSize / 8.0f);
    return surface.z;
}

bool CollisionWorld::terrainHoleAt(float x, float y) const {
    float fracX = 0.0f, fracY = 0.0f;
    const auto* chunk = findChunkAt(x, y, fracX, fracY);
    if (!chunk || chunk->holes == 0) return false;
    // The quad, read from the same two fractions the height sampler takes, so
    // the surface stood on and the surface drawn agree on which quads exist.
    const int qy = glm::clamp(static_cast<int>(std::floor(fracY)), 0, 7);
    const int qx = glm::clamp(static_cast<int>(std::floor(fracX)), 0, 7);
    return chunk->isHole(qy, qx);
}

bool CollisionWorld::terrainChunkHasHoles(float x, float y) const {
    float fracX = 0.0f, fracY = 0.0f;
    const auto* chunk = findChunkAt(x, y, fracX, fracY);
    return chunk != nullptr && chunk->holes != 0;
}

bool CollisionWorld::hasTerrainAt(float x, float y) const {
    const auto [tileX, tileY] = core::coords::worldToTile(x, y);
    return terrain_.count(tileKey(tileX, tileY)) != 0;
}

std::optional<uint32_t> CollisionWorld::areaIdAt(float x, float y) const {
    float fracX = 0.0f, fracY = 0.0f;
    const auto* chunk = findChunkAt(x, y, fracX, fracY);
    if (!chunk || chunk->areaId == 0) return std::nullopt;
    return chunk->areaId;
}

// ---- Building queries ------------------------------------------------------------

std::optional<float> CollisionWorld::wmoFloor(const Reach& reach, float x, float y, float z,
                                              float* outNormalZ, float referenceZ,
                                              bool* outFacesUp) const {
    const bool byReference = std::isfinite(referenceZ);
    QueryTimer timer(wmoQueryMs_, wmoQueryCalls_);
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;
    bool bestFacesUp = true;
    // For the "no floor where there plainly is one" report at the end.
    bool overlappedInXY = false;
    bool rejectedByZ = false;
    float rejectedZMin = 0.0f, rejectedZMax = 0.0f;

    // A transport deck counts only when it is genuinely underfoot, not
    // anywhere along its shaft. referenceZ is the true feet when the caller
    // supplies one; otherwise z stands in, and that is the probe rather than
    // the feet - the caller raised it by its step-up budget - so back that
    // lift out first.
    constexpr float kTransportProbeLift = 2.0f;
    constexpr float kTransportFloorReach = 3.0f;
    const float transportFloorMinZ =
        (byReference ? referenceZ : z - kTransportProbeLift) - kTransportFloorReach;

    // World-space ray: from high above, straight down.
    const glm::vec3 worldOrigin(x, y, z + 500.0f);
    const glm::vec3 worldDir(0.0f, 0.0f, -1.0f);

    std::vector<const WmoPlacement*> candidates;
    gatherWmo(glm::vec3(x - 2.0f, y - 2.0f, z - 8.0f), glm::vec3(x + 2.0f, y + 2.0f, z + 10.0f),
              candidates);

    for (const WmoPlacement* placement : candidates) {
        const WmoPlacement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;
        const WmoShape& shape = *p.shape;

        const float zMarginDown = shape.isLowPlatform ? 20.0f : 2.0f;
        const float zMarginUp = shape.isLowPlatform ? 20.0f : 4.0f;

        // Broad phase in world space, before any matrix work.
        // Highest-floor pruning cannot serve a nearest-to-feet query. Nor
        // may a tolerance discard a distinct, slightly higher surface.
        if (!byReference && bestFloor && p.boundsMax.z <= *bestFloor) continue;
        const bool insideXY = x >= p.boundsMin.x && x <= p.boundsMax.x &&
                              y >= p.boundsMin.y && y <= p.boundsMax.y;
        if (!withinBounds(p.boundsMin, p.boundsMax, x, y, z, zMarginDown, zMarginUp)) {
            // Over a building and rejected anyway: only the Z window does
            // that, and it is worth naming when the answer is "no floor".
            if (insideXY) {
                overlappedInXY = true;
                rejectedByZ = true;
                rejectedZMin = p.boundsMin.z;
                rejectedZMax = p.boundsMax.z;
            }
            continue;
        }
        if (insideXY) overlappedInXY = true;

        // Which groups' world XY boxes contain the point: for a vertical ray
        // this drops most groups before any local-space math.
        bool anyGroupOverlaps = false;
        for (const auto& [gMin, gMax] : p.groupBounds) {
            if (x >= gMin.x && x <= gMax.x && y >= gMin.y && y <= gMax.y && z - 4.0f <= gMax.z) {
                anyGroupOverlaps = true;
                break;
            }
        }
        if (!anyGroupOverlaps) continue;

        const glm::vec3 localOrigin = glm::vec3(p.inverse * glm::vec4(worldOrigin, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(p.inverse * glm::vec4(worldDir, 0.0f)));

        for (size_t gi = 0; gi < shape.groups.size(); ++gi) {
            const auto& [gMin, gMax] = p.groupBounds[gi];
            if (x < gMin.x || x > gMax.x || y < gMin.y || y > gMax.y || z - 4.0f > gMax.z) continue;

            const WmoGroupShape& group = *shape.groups[gi];
            if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) {
                continue;
            }
            // Every triangle the ray crosses, floors or not: a vertical ray
            // misses vertical geometry by itself, and a normal filter risks
            // dropping steep ramps and stair treads.
            if (!group.trianglesAlongRay(localOrigin, localDir, tl_triScratch)) continue;

            for (uint32_t triStart : tl_triScratch) {
                const glm::vec3& v0 = group.vertices[group.indices[triStart]];
                const glm::vec3& v1 = group.vertices[group.indices[triStart + 1]];
                const glm::vec3& v2 = group.vertices[group.indices[triStart + 2]];
                const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
                if (t <= 0.0f) continue;

                const glm::vec3 hitLocal = localOrigin + localDir * t;
                const glm::vec3 hitWorld = glm::vec3(p.transform * glm::vec4(hitLocal, 1.0f));
                // At or below z (already raised by the step-up budget); among
                // those the highest, or the nearest the feet.
                if (hitWorld.z > z || (p.isTransport && hitWorld.z < transportFloorMinZ)) continue;
                const bool better = !bestFloor
                    ? true
                    : (byReference ? std::abs(hitWorld.z - referenceZ) < std::abs(*bestFloor - referenceZ)
                                   : hitWorld.z > *bestFloor);
                if (!better) continue;
                bestFloor = hitWorld.z;
                glm::vec3 localNormal = group.triNormals[triStart / 3];
                const bool flipped = localNormal.z < 0.0f;
                if (flipped) localNormal = -localNormal;
                const glm::vec3 worldNormal = glm::normalize(
                    glm::vec3(p.transform * glm::vec4(localNormal, 0.0f)));
                bestNormalZ = std::abs(worldNormal.z);
                bestFacesUp = (worldNormal.z > 0.0f) != flipped;
            }
        }
    }

    // Standing over a building and told there is no floor. Falling through
    // looks the same whether the building was never considered, rejected by
    // its height window or missed by the ray, and those need different fixes.
    // Once per place, a couple of units across, and not past a cap: a run
    // finding hundreds has one problem, not hundreds.
    if (!bestFloor && overlappedInXY) {
        struct ReportedPlace { float x, y, z; };
        static std::mutex reportedMutex;
        static std::vector<ReportedPlace> reported;
        static bool reportedCap = false;
        constexpr float kSamePlace = 2.0f;
        constexpr size_t kMaxPlaces = 32;
        std::lock_guard<std::mutex> lock(reportedMutex);
        const bool seenHere = std::any_of(reported.begin(), reported.end(), [&](const ReportedPlace& r) {
            const float dx = r.x - x, dy = r.y - y, dz = r.z - z;
            return dx * dx + dy * dy + dz * dz <= kSamePlace * kSamePlace;
        });
        if (!seenHere && reported.size() >= kMaxPlaces) {
            if (!reportedCap) {
                reportedCap = true;
                LOG_WARNING("No WMO floor: ", kMaxPlaces, " places reported and no more "
                            "will be - this is one problem, not ", kMaxPlaces, " of them");
            }
        } else if (!seenHere) {
            reported.push_back({x, y, z});
            if (rejectedByZ) {
                LOG_WARNING("No WMO floor at (", x, ",", y, ",", z,
                            ") - a building covers that spot but its height window "
                            "rejected the query: bounds z=[", rejectedZMin, "..", rejectedZMax, "]");
            } else {
                LOG_WARNING("No WMO floor at (", x, ",", y, ",", z,
                            ") - a building covers that spot, was considered, and "
                            "no triangle was hit");
            }
        }
    }

    if (bestFloor && outNormalZ) *outNormalZ = bestNormalZ;
    if (bestFloor && outFacesUp) *outFacesUp = bestFacesUp;
    return bestFloor;
}

std::optional<float> CollisionWorld::wmoInstanceFloor(uint32_t instanceId, const glm::mat4& placement,
                                                      float x, float y, float z,
                                                      float* outNormalZ) const {
    const auto it = wmo_.find(instanceId);
    if (it == wmo_.end()) return std::nullopt;
    const WmoShape& shape = *it->second.shape;

    // Where its owner has it this frame: the boxes kept with the placement
    // are where it last stood, so only the groups' own boxes cut this short.
    const glm::mat4 inverse = glm::inverse(placement);
    const glm::vec3 localOrigin(inverse * glm::vec4(x, y, z + 500.0f, 1.0f));
    const glm::vec3 localDir = glm::normalize(glm::vec3(inverse * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;
    for (const auto& groupPtr : shape.groups) {
        const WmoGroupShape& group = *groupPtr;
        if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) continue;
        if (!group.trianglesAlongRay(localOrigin, localDir, tl_triScratch)) continue;
        for (uint32_t triStart : tl_triScratch) {
            const glm::vec3& v0 = group.vertices[group.indices[triStart]];
            const glm::vec3& v1 = group.vertices[group.indices[triStart + 1]];
            const glm::vec3& v2 = group.vertices[group.indices[triStart + 2]];
            const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
            if (t <= 0.0f) continue;
            const glm::vec3 hitWorld(placement * glm::vec4(localOrigin + localDir * t, 1.0f));
            if (hitWorld.z > z || (bestFloor && hitWorld.z <= *bestFloor)) continue;
            bestFloor = hitWorld.z;
            glm::vec3 localNormal = group.triNormals[triStart / 3];
            if (localNormal.z < 0.0f) localNormal = -localNormal;
            bestNormalZ = std::abs(glm::normalize(glm::vec3(placement * glm::vec4(localNormal, 0.0f))).z);
        }
    }
    if (bestFloor && outNormalZ) *outNormalZ = bestNormalZ;
    return bestFloor;
}

bool CollisionWorld::hasWmo(uint32_t instanceId) const {
    return wmo_.count(instanceId) != 0;
}

bool CollisionWorld::wmoSegmentBlocked(const Reach& reach, const glm::vec3& from,
                                       const glm::vec3& to) const {
    QueryTimer timer(wmoQueryMs_, wmoQueryCalls_);
    const glm::vec3 delta = to - from;
    const float dist = glm::length(delta);
    if (dist < 1e-4f) return false;
    const glm::vec3 dir = delta / dist;

    // A metre of slack: a wall's instance box can begin just outside the
    // segment's.
    std::vector<const WmoPlacement*> candidates;
    gatherWmo(glm::min(from, to) - glm::vec3(1.0f), glm::max(from, to) + glm::vec3(1.0f), candidates);

    for (const WmoPlacement* placement : candidates) {
        const WmoPlacement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;
        if (!rayIntersectsAABB(from, dir, p.boundsMin, p.boundsMax)) continue;
        const WmoShape& shape = *p.shape;

        // In the instance's own space, distances too: a scaled placement
        // makes local and world units different lengths.
        const glm::vec3 localFrom = glm::vec3(p.inverse * glm::vec4(from, 1.0f));
        const glm::vec3 localTo = glm::vec3(p.inverse * glm::vec4(to, 1.0f));
        const glm::vec3 localDelta = localTo - localFrom;
        const float localDist = glm::length(localDelta);
        if (localDist < 1e-4f) continue;
        const glm::vec3 localDir = localDelta / localDist;
        // Near the eye a camera clipped into geometry would call everything
        // blocked; at the far end a player against a wall is in front of it.
        const float nearMargin = 0.15f * (localDist / dist);
        const float farMargin = 0.35f * (localDist / dist);

        for (size_t gi = 0; gi < shape.groups.size(); ++gi) {
            const auto& [gMin, gMax] = p.groupBounds[gi];
            if (!rayIntersectsAABB(from, dir, gMin, gMax)) continue;
            const WmoGroupShape& group = *shape.groups[gi];
            if (!group.trianglesAlongRay(localFrom, localDir, tl_triScratch)) continue;
            for (uint32_t triStart : tl_triScratch) {
                if (triStart + 2 >= group.indices.size()) continue;
                const float t = rayTriangleIntersect(localFrom, localDir,
                                                     group.vertices[group.indices[triStart]],
                                                     group.vertices[group.indices[triStart + 1]],
                                                     group.vertices[group.indices[triStart + 2]]);
                if (t > nearMargin && t < localDist - farMargin) return true;
            }
        }
    }
    return false;
}

bool CollisionWorld::wmoWallSlide(const Reach& reach, const glm::vec3& from, const glm::vec3& to,
                                  glm::vec3& adjusted, bool insideWmo) const {
    QueryTimer timer(wmoQueryMs_, wmoQueryCalls_);
    adjusted = to;
    bool blocked = false;

    const glm::vec3 moveDir = to - from;
    if (glm::dot(moveDir, moveDir) < 1e-6f) return false;

    // A WoW-style upright cylinder, a little tighter indoors.
    const float kRadius = insideWmo ? 0.45f : 0.50f;
    constexpr float kHeight = 2.0f;
    constexpr float kMaxStepHeight = 1.0f;

    std::vector<const WmoPlacement*> candidates;
    gatherWmo(glm::min(from, to) - glm::vec3(8.0f, 8.0f, 5.0f),
              glm::max(from, to) + glm::vec3(8.0f, 8.0f, 5.0f), candidates);

    for (const WmoPlacement* placement : candidates) {
        const WmoPlacement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;

        const float broadMargin = kRadius + 1.5f;
        if (from.x < p.boundsMin.x - broadMargin && to.x < p.boundsMin.x - broadMargin) continue;
        if (from.x > p.boundsMax.x + broadMargin && to.x > p.boundsMax.x + broadMargin) continue;
        if (from.y < p.boundsMin.y - broadMargin && to.y < p.boundsMin.y - broadMargin) continue;
        if (from.y > p.boundsMax.y + broadMargin && to.y > p.boundsMax.y + broadMargin) continue;
        if (from.z > p.boundsMax.z + kHeight && to.z > p.boundsMax.z + kHeight) continue;
        if (from.z + kHeight < p.boundsMin.z && to.z + kHeight < p.boundsMin.z) continue;

        const WmoShape& shape = *p.shape;
        const float wallMargin = kRadius + 2.0f;
        bool anyGroupNear = false;
        for (const auto& [gMin, gMax] : p.groupBounds) {
            if (to.x >= gMin.x - wallMargin && to.x <= gMax.x + wallMargin &&
                to.y >= gMin.y - wallMargin && to.y <= gMax.y + wallMargin &&
                to.z + kHeight >= gMin.z && to.z <= gMax.z + wallMargin) {
                anyGroupNear = true;
                break;
            }
        }
        if (!anyGroupNear) continue;

        const glm::vec3 localFrom = glm::vec3(p.inverse * glm::vec4(from, 1.0f));
        glm::vec3 localTo = glm::vec3(p.inverse * glm::vec4(to, 1.0f));
        const float localFeetZ = localTo.z;
        for (size_t gi = 0; gi < shape.groups.size(); ++gi) {
            const auto& [gMin, gMax] = p.groupBounds[gi];
            if (to.x < gMin.x - wallMargin || to.x > gMax.x + wallMargin ||
                to.y < gMin.y - wallMargin || to.y > gMax.y + wallMargin ||
                to.z > gMax.z + kHeight || to.z + kHeight < gMin.z) {
                continue;
            }

            const WmoGroupShape& group = *shape.groups[gi];
            const float margin = kRadius + 2.0f;
            if (localTo.x < group.boundingBoxMin.x - margin || localTo.x > group.boundingBoxMax.x + margin ||
                localTo.y < group.boundingBoxMin.y - margin || localTo.y > group.boundingBoxMax.y + margin ||
                localTo.z < group.boundingBoxMin.z - margin || localTo.z > group.boundingBoxMax.z + margin) {
                continue;
            }

            group.trianglesInRange(std::min(localFrom.x, localTo.x) - kRadius - 1.5f,
                                   std::min(localFrom.y, localTo.y) - kRadius - 1.5f,
                                   std::max(localFrom.x, localTo.x) + kRadius + 1.5f,
                                   std::max(localFrom.y, localTo.y) + kRadius + 1.5f, tl_triScratch);

            for (uint32_t triStart : tl_triScratch) {
                const auto& tb = group.triBounds[triStart / 3];
                // Only walls in the mover's vertical span.
                if (tb.maxZ < localFeetZ + 0.3f) continue;
                if (tb.minZ > localFeetZ + kHeight) continue;
                // Low enough to step over.
                if (tb.maxZ <= localFeetZ + kMaxStepHeight) continue;
                // Short vertical faces: stair risers.
                const float triHeight = tb.maxZ - tb.minZ;
                if (triHeight < 1.0f && tb.maxZ <= localFeetZ + 1.2f) continue;

                // What blocks, by MOPY: an explicit collision hull (0x08),
                // rendered or not - tunnel walls rely on invisible hulls - and
                // rendered geometry (0x20) that is not detail (0x04): the
                // Deeprun Tram gates carry render flags without 0x08. Detail
                // never blocks - unless it is all the group has.
                const uint32_t triIdx = triStart / 3;
                if (!group.noBlockingTriangles && triIdx < group.triMopyFlags.size()) {
                    const uint8_t mopy = group.triMopyFlags[triIdx];
                    if (mopy != 0) {
                        const bool collisionHull = (mopy & 0x08) != 0;
                        const bool renderedSolid = (mopy & 0x20) != 0 && !(mopy & 0x04);
                        if (!collisionHull && !renderedSolid) continue;
                    }
                }

                const glm::vec3& v0 = group.vertices[group.indices[triStart]];
                const glm::vec3& v1 = group.vertices[group.indices[triStart + 1]];
                const glm::vec3& v2 = group.vertices[group.indices[triStart + 2]];
                const glm::vec3 normal = group.triNormals[triIdx];
                if (glm::dot(normal, normal) < 0.5f) continue;  // degenerate

                const float fromDist = glm::dot(localFrom - v0, normal);
                const float toDist = glm::dot(localTo - v0, normal);

                // Swept: crossing the wall's plane between frames, through it.
                if ((fromDist > kRadius && toDist < -kRadius) ||
                    (fromDist < -kRadius && toDist > kRadius)) {
                    const float denom = fromDist - toDist;
                    if (std::abs(denom) > 1e-6f) {
                        const float tHit = fromDist / denom;
                        if (tHit >= 0.0f && tHit <= 1.0f) {
                            const glm::vec3 hitPoint = localFrom + (localTo - localFrom) * tHit;
                            const glm::vec3 hitClosest = closestPointOnTriangle(hitPoint, v0, v1, v2);
                            if (glm::dot(hitClosest - hitPoint, hitClosest - hitPoint) <= 0.25f * 0.25f) {
                                const float side = fromDist > 0.0f ? 1.0f : -1.0f;
                                const glm::vec3 safeLocal = hitPoint + normal * side * (kRadius + 0.05f);
                                glm::vec3 pushLocal(safeLocal.x - localTo.x, safeLocal.y - localTo.y, 0.0f);
                                // Capped, so a wall does not shove the mover violently.
                                const float pushLenSq = pushLocal.x * pushLocal.x + pushLocal.y * pushLocal.y;
                                const float maxSweptPush = insideWmo ? 0.45f : 0.25f;
                                if (pushLenSq > maxSweptPush * maxSweptPush) {
                                    const float scale = maxSweptPush * glm::inversesqrt(pushLenSq);
                                    pushLocal.x *= scale;
                                    pushLocal.y *= scale;
                                }
                                localTo.x += pushLocal.x;
                                localTo.y += pushLocal.y;
                                const glm::vec3 pushWorld = glm::vec3(p.transform * glm::vec4(pushLocal, 0.0f));
                                adjusted.x += pushWorld.x;
                                adjusted.y += pushWorld.y;
                                blocked = true;
                                continue;
                            }
                        }
                    }
                }

                // The cylinder against the triangle: nearest point, sideways
                // distance.
                const glm::vec3 closest = closestPointOnTriangle(localTo, v0, v1, v2);
                const glm::vec3 d = localTo - closest;
                const float horizDistSq = d.x * d.x + d.y * d.y;
                if (horizDistSq > kRadius * kRadius) continue;
                // A floor-like surface is the grounding's, not the wall's.
                if (std::abs(normal.z) >= WmoGroupShape::kWallMaxAbsNormalZ) continue;

                constexpr float kSkin = 0.005f;
                const float horizDist = std::sqrt(horizDistSq);
                // The whole penetration, or the mover clips through gradually.
                const float pushDist = glm::clamp(kRadius - horizDist + kSkin, 0.0f, kRadius);
                glm::vec2 pushDir2;
                if (horizDistSq > 1e-8f) {
                    pushDir2 = glm::vec2(d.x, d.y) * (1.0f / horizDist);
                } else {
                    const glm::vec2 n2(normal.x, normal.y);
                    const float n2LenSq = glm::dot(n2, n2);
                    if (n2LenSq < 1e-8f) continue;
                    pushDir2 = n2 * glm::inversesqrt(n2LenSq);
                }
                const glm::vec3 pushLocal(pushDir2.x * pushDist, pushDir2.y * pushDist, 0.0f);
                localTo.x += pushLocal.x;
                localTo.y += pushLocal.y;
                const glm::vec3 pushWorld = glm::vec3(p.transform * glm::vec4(pushLocal, 0.0f));
                adjusted.x += pushWorld.x;
                adjusted.y += pushWorld.y;
                blocked = true;
            }
        }
    }
    return blocked;
}

float CollisionWorld::wmoRaycast(const Reach& reach, const glm::vec3& origin,
                                 const glm::vec3& direction, float maxDistance, bool cameraQuery) const {
    QueryTimer timer(wmoQueryMs_, wmoQueryCalls_);
    float closestHit = maxDistance;
    // The camera is a solid sphere and every solid surface stops it, floors
    // and ceilings as much as walls. A surface within a hand's width of the
    // pivot is the one the player stands in, and would jam the camera.
    const float kMinHitDistance = cameraQuery ? 0.20f : 0.0f;
    // Only surfaces the ray is very nearly parallel to are dropped: a wall
    // approached at a shallow angle is exactly the one the camera slides
    // along and is about to pass through.
    const float kMinSurfaceAlignment = cameraQuery ? 0.05f : 0.0f;

    const glm::vec3 rayEnd = origin + direction * maxDistance;
    std::vector<const WmoPlacement*> candidates;
    gatherWmo(glm::min(origin, rayEnd) - glm::vec3(1.0f), glm::max(origin, rayEnd) + glm::vec3(1.0f),
              candidates);

    for (const WmoPlacement* placement : candidates) {
        const WmoPlacement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;

        const glm::vec3 center = (p.boundsMin + p.boundsMax) * 0.5f;
        const glm::vec3 halfExtent = p.boundsMax - center;
        const glm::vec3 toCenter = center - origin;
        const float maxR = maxDistance + std::sqrt(glm::dot(halfExtent, halfExtent)) + 1.0f;
        if (glm::dot(toCenter, toCenter) > maxR * maxR) continue;
        if (!rayIntersectsAABB(origin, direction, p.boundsMin - glm::vec3(0.5f),
                               p.boundsMax + glm::vec3(0.5f))) {
            continue;
        }

        const WmoShape& shape = *p.shape;
        const glm::vec3 localOrigin = glm::vec3(p.inverse * glm::vec4(origin, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(p.inverse * glm::vec4(direction, 0.0f)));

        for (size_t gi = 0; gi < shape.groups.size(); ++gi) {
            const auto& [gMin, gMax] = p.groupBounds[gi];
            if (!rayIntersectsAABB(origin, direction, gMin, gMax)) continue;
            const WmoGroupShape& group = *shape.groups[gi];
            if (!rayIntersectsAABB(localOrigin, localDir, group.boundingBoxMin, group.boundingBoxMax)) continue;

            const glm::vec3 localEnd = localOrigin + localDir * (closestHit / glm::length(
                glm::vec3(p.transform * glm::vec4(localDir, 0.0f))));
            group.trianglesInRange(std::min(localOrigin.x, localEnd.x) - 1.0f,
                                   std::min(localOrigin.y, localEnd.y) - 1.0f,
                                   std::max(localOrigin.x, localEnd.x) + 1.0f,
                                   std::max(localOrigin.y, localEnd.y) + 1.0f, tl_triScratch);

            for (uint32_t triStart : tl_triScratch) {
                const glm::vec3& v0 = group.vertices[group.indices[triStart]];
                const glm::vec3& v1 = group.vertices[group.indices[triStart + 1]];
                const glm::vec3& v2 = group.vertices[group.indices[triStart + 2]];
                const glm::vec3 triNormal = group.triNormals[triStart / 3];
                if (glm::dot(triNormal, triNormal) < 0.5f) continue;  // degenerate
                if (std::abs(glm::dot(triNormal, localDir)) < kMinSurfaceAlignment) continue;

                const float t = rayTriangleIntersect(localOrigin, localDir, v0, v1, v2);
                if (t <= 0.0f) continue;
                const glm::vec3 worldHit = glm::vec3(p.transform * glm::vec4(localOrigin + localDir * t, 1.0f));
                // No height band around the pivot: the camera orbits and
                // pitches, so the far end of this ray is normally metres above
                // or below it, and a band dropped exactly the walls it was to
                // find.
                const float worldDist = glm::length(worldHit - origin);
                if (worldDist < kMinHitDistance) continue;
                if (worldDist < closestHit && worldDist <= maxDistance) closestHit = worldDist;
            }
        }
    }
    return closestHit;
}

std::optional<CollisionWorld::GroupHolding> CollisionWorld::findGroupHolding(
        const Reach& reach, float x, float y, float z, bool interiorOnly) const {
    std::vector<const WmoPlacement*> candidates;
    gatherWmo(glm::vec3(x - 0.5f, y - 0.5f, z - 0.5f), glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f),
              candidates);
    for (const WmoPlacement* placement : candidates) {
        const WmoPlacement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;
        if (!withinBounds(p.boundsMin, p.boundsMax, x, y, z)) continue;

        // A building whose own box holds the point usually has no group that
        // does, and the world boxes say so before the transform.
        bool anyGroupContains = false;
        for (const auto& [gMin, gMax] : p.groupBounds) {
            if (x >= gMin.x && x <= gMax.x && y >= gMin.y && y <= gMax.y && z >= gMin.z && z <= gMax.z) {
                anyGroupContains = true;
                break;
            }
        }
        if (!anyGroupContains) continue;

        const glm::vec3 localPos = glm::vec3(p.inverse * glm::vec4(x, y, z, 1.0f));
        for (const auto& group : p.shape->groups) {
            if (groupContainsPoint(*group, localPos, interiorOnly)) {
                return GroupHolding{.placement = &p, .group = group.get(), .localPos = localPos};
            }
        }
    }
    return std::nullopt;
}

bool CollisionWorld::insideWmo(const Reach& reach, float x, float y, float z,
                               uint32_t* outModelId) const {
    QueryTimer timer(wmoQueryMs_, wmoQueryCalls_);
    const auto holding = findGroupHolding(reach, x, y, z, /*interiorOnly=*/false);
    if (holding && outModelId) *outModelId = holding->placement->shape->modelId;
    return holding.has_value();
}

bool CollisionWorld::insideInteriorWmo(const Reach& reach, float x, float y, float z) const {
    return findGroupHolding(reach, x, y, z, /*interiorOnly=*/true).has_value();
}

std::optional<CollisionWorld::InteriorSample> CollisionWorld::interiorAt(float x, float y,
                                                                         float z) const {
    QueryTimer timer(wmoQueryMs_, wmoQueryCalls_);
    const auto holding = findGroupHolding(Reach::everywhere(), x, y, z, /*interiorOnly=*/true);
    if (!holding) return std::nullopt;

    InteriorSample sample;
    sample.ambient = holding->placement->shape->ambientColor;

    // The vertex colour of the nearest surface straight below.
    const WmoGroupShape& group = *holding->group;
    if (!group.vertexColors.empty()) {
        const glm::vec3 localDown = glm::normalize(
            glm::vec3(holding->placement->inverse * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        if (group.trianglesAlongRay(holding->localPos, localDown, tl_triScratch)) {
            float nearest = std::numeric_limits<float>::max();
            for (uint32_t triStart : tl_triScratch) {
                const uint16_t i0 = group.indices[triStart];
                const uint16_t i1 = group.indices[triStart + 1];
                const uint16_t i2 = group.indices[triStart + 2];
                const float t = rayTriangleIntersect(holding->localPos, localDown, group.vertices[i0],
                                                     group.vertices[i1], group.vertices[i2]);
                if (t <= 0.0f || t >= nearest) continue;
                nearest = t;
                sample.floorColor = colorAtPoint(holding->localPos + localDown * t, group.vertices[i0],
                                                 group.vertices[i1], group.vertices[i2],
                                                 group.vertexColors[i0], group.vertexColors[i1],
                                                 group.vertexColors[i2]);
            }
        }
    }
    return sample;
}

// ---- Doodad queries --------------------------------------------------------------

std::optional<float> CollisionWorld::m2Floor(const Reach& reach, float x, float y, float z,
                                             float* outNormalZ) const {
    QueryTimer timer(m2QueryMs_, m2QueryCalls_);
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;

    std::vector<const M2Placement*> candidates;
    gatherM2(glm::vec3(x - 2.0f, y - 2.0f, z - 6.0f), glm::vec3(x + 2.0f, y + 2.0f, z + 8.0f), candidates);

    for (const M2Placement* placement : candidates) {
        const M2Placement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;
        if (p.scale <= 0.001f || p.skipCollision) continue;
        const M2Shape& shape = *p.shape;

        // World-down through the placement: local -Z is down only for
        // yaw-only placements, and a pitched flat model (the Azuremyst
        // flight-master ramp) would otherwise see a diagonal ray.
        const glm::vec3 localRayOrigin = glm::vec3(p.inverse * glm::vec4(x, y, z + 5.0f, 1.0f));
        const glm::vec3 localRayEnd = glm::vec3(p.inverse * glm::vec4(x, y, z - 10.0f, 1.0f));
        const glm::vec3 localRayVector = localRayEnd - localRayOrigin;
        const float localRayLength = glm::length(localRayVector);
        if (localRayLength <= 1e-5f) continue;
        const glm::vec3 localRayDir = localRayVector / localRayLength;

        shape.trisInRange(std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
                               std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
                               std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
                               std::max(localRayOrigin.y, localRayEnd.y) + 1.0f, tl_triScratch);

        float bestWorldHitZ = -std::numeric_limits<float>::max();
        float bestHitNormalZ = 1.0f;
        bool hitAny = false;
        for (uint32_t ti : tl_triScratch) {
            if (ti >= shape.triCount) continue;
            const auto& v0 = shape.vertices[shape.indices[ti * 3]];
            const auto& v1 = shape.vertices[shape.indices[ti * 3 + 1]];
            const auto& v2 = shape.vertices[shape.indices[ti * 3 + 2]];
            const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2);
            if (tHit < 0.0f || tHit > localRayLength) continue;
            const glm::vec3 worldHit = glm::vec3(p.transform * glm::vec4(localRayOrigin + localRayDir * tHit, 1.0f));

            // Walkable: about 70 degrees at most, measured in the world.
            glm::vec3 worldN(0.0f, 0.0f, 1.0f);
            glm::vec3 localN = glm::cross(v1 - v0, v2 - v0);
            const float nLen = glm::length(localN);
            if (nLen > 0.001f) {
                localN /= nLen;
                if (localN.z < 0.0f) localN = -localN;
                const glm::vec3 transformedN = glm::vec3(p.transform * glm::vec4(localN, 0.0f));
                const float wnLen = glm::length(transformedN);
                if (wnLen > 0.001f) worldN = transformedN / wnLen;
                if (std::abs(worldN.z) < 0.35f) continue;
            }
            if (worldHit.z <= z + 3.0f && worldHit.z > bestWorldHitZ) {
                bestWorldHitZ = worldHit.z;
                hitAny = true;
                bestHitNormalZ = std::abs(worldN.z);
            }
        }
        if (hitAny && (!bestFloor || bestWorldHitZ > *bestFloor)) {
            bestFloor = bestWorldHitZ;
            bestNormalZ = bestHitNormalZ;
        }
    }
    if (outNormalZ) *outNormalZ = bestNormalZ;
    return bestFloor;
}

std::optional<float> CollisionWorld::m2InstanceFloor(uint32_t instanceId, const glm::mat4& placement,
                                                     float x, float y, float z) const {
    const auto it = m2_.find(instanceId);
    if (it == m2_.end()) return std::nullopt;
    const M2Placement& p = it->second;
    // Out of the world, it is nobody's floor.
    if (p.hidden || p.scale <= 0.001f) return std::nullopt;
    const M2Shape& shape = *p.shape;

    const glm::mat4 inverse = glm::inverse(placement);
    const glm::vec3 localRayOrigin = glm::vec3(inverse * glm::vec4(x, y, z + 5.0f, 1.0f));
    const glm::vec3 localRayEnd = glm::vec3(inverse * glm::vec4(x, y, z - 10.0f, 1.0f));
    const glm::vec3 localRayVector = localRayEnd - localRayOrigin;
    const float localRayLength = glm::length(localRayVector);
    if (localRayLength <= 1e-5f) return std::nullopt;
    const glm::vec3 localRayDir = localRayVector / localRayLength;

    shape.trisInRange(std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
                           std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
                           std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
                           std::max(localRayOrigin.y, localRayEnd.y) + 1.0f, tl_triScratch);
    std::optional<float> best;
    for (uint32_t ti : tl_triScratch) {
        if (ti >= shape.triCount) continue;
        const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir,
                                                shape.vertices[shape.indices[ti * 3]],
                                                shape.vertices[shape.indices[ti * 3 + 1]],
                                                shape.vertices[shape.indices[ti * 3 + 2]]);
        if (tHit < 0.0f || tHit > localRayLength) continue;
        const glm::vec3 worldHit = glm::vec3(placement * glm::vec4(localRayOrigin + localRayDir * tHit, 1.0f));
        // At or under the probe, and the highest such - the deck rather than
        // whatever structure the car carries beneath it.
        if (worldHit.z <= z && (!best || worldHit.z > *best)) best = worldHit.z;
    }
    return best;
}

bool CollisionWorld::m2InstanceBounds(uint32_t instanceId, const glm::mat4& placement,
                                      glm::vec3& outMin, glm::vec3& outMax) const {
    const auto it = m2_.find(instanceId);
    if (it == m2_.end()) return false;
    transformAABB(placement, it->second.shape->boundMin, it->second.shape->boundMax, outMin, outMax);
    return true;
}

bool CollisionWorld::m2WallSlide(const Reach& reach, const glm::vec3& from, const glm::vec3& to,
                                 glm::vec3& adjusted, float radius) const {
    QueryTimer timer(m2QueryMs_, m2QueryCalls_);
    adjusted = to;
    bool collided = false;

    std::vector<const M2Placement*> candidates;
    gatherM2(glm::min(from, to) - glm::vec3(7.0f, 7.0f, 5.0f),
             glm::max(from, to) + glm::vec3(7.0f, 7.0f, 5.0f), candidates);

    for (const M2Placement* placement : candidates) {
        const M2Placement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;

        const float broadMargin = radius + 1.0f;
        if (from.x < p.boundsMin.x - broadMargin && adjusted.x < p.boundsMin.x - broadMargin) continue;
        if (from.x > p.boundsMax.x + broadMargin && adjusted.x > p.boundsMax.x + broadMargin) continue;
        if (from.y < p.boundsMin.y - broadMargin && adjusted.y < p.boundsMin.y - broadMargin) continue;
        if (from.y > p.boundsMax.y + broadMargin && adjusted.y > p.boundsMax.y + broadMargin) continue;
        if (from.z > p.boundsMax.z + 2.5f && adjusted.z > p.boundsMax.z + 2.5f) continue;
        if (from.z + 2.5f < p.boundsMin.z && adjusted.z + 2.5f < p.boundsMin.z) continue;

        if (p.skipCollision || p.skipWalls || p.scale <= 0.001f) continue;
        const M2Shape& shape = *p.shape;

        const glm::vec3 localFrom = glm::vec3(p.inverse * glm::vec4(from, 1.0f));
        glm::vec3 localPos = glm::vec3(p.inverse * glm::vec4(adjusted, 1.0f));
        const float localRadius = radius / p.scale;

        shape.wallTrisInRange(std::min(localFrom.x, localPos.x) - localRadius - 1.0f,
                              std::min(localFrom.y, localPos.y) - localRadius - 1.0f,
                              std::max(localFrom.x, localPos.x) + localRadius + 1.0f,
                              std::max(localFrom.y, localPos.y) + localRadius + 1.0f, tl_triScratch);

        constexpr float kHeight = 2.0f;
        constexpr float kMaxTotalPush = 0.02f;  // per instance
        constexpr float kMaxStepUp = 1.2f;
        bool pushed = false;
        float totalPushX = 0.0f, totalPushY = 0.0f;

        for (uint32_t ti : tl_triScratch) {
            if (ti >= shape.triCount) continue;
            if (localPos.z + kHeight < shape.triBounds[ti].minZ || localPos.z > shape.triBounds[ti].maxZ) continue;
            // A wall is only stepped over while rising, as in a jump.
            const bool rising = (localPos.z > localFrom.z + 0.05f);
            if (rising && localPos.z + kMaxStepUp >= shape.triBounds[ti].maxZ) continue;
            if (std::sqrt(totalPushX * totalPushX + totalPushY * totalPushY) >= kMaxTotalPush) break;

            const auto& v0 = shape.vertices[shape.indices[ti * 3]];
            const auto& v1 = shape.vertices[shape.indices[ti * 3 + 1]];
            const auto& v2 = shape.vertices[shape.indices[ti * 3 + 2]];
            const glm::vec3 closest = closestPointOnTriangle(localPos, v0, v1, v2);
            const glm::vec3 diff = localPos - closest;
            const float distXY = std::sqrt(diff.x * diff.x + diff.y * diff.y);

            if (distXY < localRadius && distXY > 1e-4f) {
                // A gentle push: a small fraction of the penetration.
                const float pushDist = std::clamp((localRadius - distXY) * 0.08f, 0.001f, 0.015f);
                const float dx = (diff.x / distXY) * pushDist;
                const float dy = (diff.y / distXY) * pushDist;
                localPos.x += dx;
                localPos.y += dy;
                totalPushX += dx;
                totalPushY += dy;
                pushed = true;
            } else if (distXY < 1e-4f) {
                // On the plane: along the triangle's normal in XY.
                const glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
                const float nxyLen = std::sqrt(n.x * n.x + n.y * n.y);
                if (nxyLen > 1e-4f) {
                    const float pushDist = std::min(localRadius, 0.015f);
                    const float dx = (n.x / nxyLen) * pushDist;
                    const float dy = (n.y / nxyLen) * pushDist;
                    localPos.x += dx;
                    localPos.y += dy;
                    totalPushX += dx;
                    totalPushY += dy;
                    pushed = true;
                }
            }
        }

        if (pushed) {
            const glm::vec3 worldPos = glm::vec3(p.transform * glm::vec4(localPos, 1.0f));
            adjusted.x = worldPos.x;
            adjusted.y = worldPos.y;
            collided = true;
            // Which doodad is in the way, once per model: a model that blocks
            // unexpectedly is reported as "I am stuck on this", and its name
            // is what finds its collision mesh.
            static std::mutex saidMutex;
            static std::set<std::string> saidBlocked;
            std::lock_guard<std::mutex> lock(saidMutex);
            if (!shape.name.empty() && saidBlocked.insert(shape.name).second) {
                LOG_INFO("Collision: blocked by '", shape.name, "' (its collision mesh)");
            }
        }
    }
    return collided;
}

float CollisionWorld::m2Raycast(const Reach& reach, const glm::vec3& origin,
                                const glm::vec3& direction, float maxDistance) const {
    // The client's world line test with doodad collision: the segment
    // against each doodad's collision triangles - not its bounding box, which
    // stopped the camera at the box around a tree's crown.
    QueryTimer timer(m2QueryMs_, m2QueryCalls_);
    float closestHit = maxDistance;

    const glm::vec3 rayEnd = origin + direction * maxDistance;
    std::vector<const M2Placement*> candidates;
    gatherM2(glm::min(origin, rayEnd) - glm::vec3(1.0f), glm::max(origin, rayEnd) + glm::vec3(1.0f),
             candidates);

    for (const M2Placement* placement : candidates) {
        const M2Placement& p = *placement;
        if (reach.excludes(p.boundsMin, p.boundsMax)) continue;
        if (p.scale <= 0.001f) continue;
        const M2Shape& shape = *p.shape;

        float tEnter = 0.0f;
        if (!segmentIntersectsAABB(origin, origin + direction * closestHit, p.boundsMin, p.boundsMax, tEnter)) {
            continue;
        }
        // In model space; the segment's parameter is the same in both.
        const glm::vec3 localOrigin = glm::vec3(p.inverse * glm::vec4(origin, 1.0f));
        const glm::vec3 localEnd = glm::vec3(p.inverse * glm::vec4(origin + direction * closestHit, 1.0f));
        const glm::vec3 localVector = localEnd - localOrigin;
        const float localLength = glm::length(localVector);
        if (localLength <= 1e-5f) continue;
        const glm::vec3 localDir = localVector / localLength;

        float nearest = localLength;
        for (uint32_t ti = 0; ti < shape.triCount; ++ti) {
            const float t = rayTriangleIntersect(localOrigin, localDir, shape.vertices[shape.indices[ti * 3]],
                                                 shape.vertices[shape.indices[ti * 3 + 1]],
                                                 shape.vertices[shape.indices[ti * 3 + 2]]);
            if (t >= 0.0f && t < nearest) nearest = t;
        }
        closestHit *= nearest / localLength;
    }
    return closestHit;
}

// ---- Movers --------------------------------------------------------------------

glm::vec3 CollisionWorld::sweepMover(const Reach& reach, const glm::vec3& from, const glm::vec3& to,
                                     bool insideWmo, bool includeDoodads) const {
    const glm::vec3 delta = to - from;
    const float distSq = glm::dot(delta, delta);
    if (distSq == 0.0f) return to;

    const float dist = std::sqrt(distSq);
    // Tighter steps indoors, where the geometry is closer together.
    const float stepSize = insideWmo ? 0.20f : 0.35f;
    const int steps = std::max(1, std::min(8, static_cast<int>(std::ceil(dist / stepSize))));
    const glm::vec3 stepDelta = delta / static_cast<float>(steps);

    glm::vec3 stepPos = from;
    for (int i = 0; i < steps; i++) {
        glm::vec3 candidate = stepPos + stepDelta;
        glm::vec3 adjusted;
        if (wmoWallSlide(reach, stepPos, candidate, adjusted, insideWmo)) {
            candidate.x = adjusted.x;
            candidate.y = adjusted.y;
            // Up is a ramp; down would be a wall pulling the feet under the floor.
            candidate.z = std::max(candidate.z, adjusted.z);
        }
        if (includeDoodads && m2WallSlide(reach, stepPos, candidate, adjusted)) {
            candidate.x = adjusted.x;
            candidate.y = adjusted.y;
        }
        stepPos = candidate;
    }
    return stepPos;
}

// ---- Diagnostics ----------------------------------------------------------------

void CollisionWorld::logFloorCandidates(float x, float y, float z) const {
    LOG_WARNING("=== Floor candidates at render(", x, ", ", y, ", ", z, ") ===");
    const auto terrain = terrainHeight(x, y);
    LOG_WARNING("  terrain: ", terrain ? std::to_string(*terrain) : std::string("none"),
                terrainHoleAt(x, y) ? " (a hole)" : "");

    // Buildings: every group under the point, the hits closest to the feet
    // from a full scan and from the grid the query uses - if they disagree,
    // the grid missed a triangle.
    const glm::vec3 worldOrigin(x, y, z + 500.0f);
    const glm::vec3 worldDir(0.0f, 0.0f, -1.0f);
    int buildings = 0;
    for (const auto& [id, p] : wmo_) {
        if (!withinBounds(p.boundsMin, p.boundsMax, x, y, z, 20.0f, 20.0f)) continue;
        ++buildings;
        const WmoShape& shape = *p.shape;
        LOG_WARNING("  WMO instance ", id, " model ", shape.modelId,
                    " transport=", p.isTransport ? 1 : 0, " hidden=", p.hidden ? 1 : 0,
                    " bounds z ", p.boundsMin.z, "..", p.boundsMax.z, " groups=", shape.groups.size());
        const glm::vec3 localOrigin = glm::vec3(p.inverse * glm::vec4(worldOrigin, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(p.inverse * glm::vec4(worldDir, 0.0f)));
        for (size_t gi = 0; gi < shape.groups.size(); ++gi) {
            const auto& [gMin, gMax] = p.groupBounds[gi];
            if (x < gMin.x || x > gMax.x || y < gMin.y || y > gMax.y) continue;
            const WmoGroupShape& group = *shape.groups[gi];
            auto closestHit = [&](const std::vector<uint32_t>& tris, int& hits) {
                float closestZ = -999999.0f, closestDist = 1e30f;
                hits = 0;
                for (uint32_t triStart : tris) {
                    if (triStart + 2 >= group.indices.size()) continue;
                    const float t = rayTriangleIntersect(localOrigin, localDir,
                                                         group.vertices[group.indices[triStart]],
                                                         group.vertices[group.indices[triStart + 1]],
                                                         group.vertices[group.indices[triStart + 2]]);
                    if (t <= 0.0f) continue;
                    ++hits;
                    const float hz = (p.transform * glm::vec4(localOrigin + localDir * t, 1.0f)).z;
                    if (std::abs(hz - z) < closestDist) { closestDist = std::abs(hz - z); closestZ = hz; }
                }
                return closestZ;
            };
            std::vector<uint32_t> all;
            for (uint32_t i = 0; i + 2 < group.indices.size(); i += 3) all.push_back(i);
            std::vector<uint32_t> grid;
            group.trianglesAlongRay(localOrigin, localDir, grid);
            int allHits = 0, gridHits = 0;
            const float allZ = closestHit(all, allHits);
            const float gridZ = closestHit(grid, gridHits);
            LOG_WARNING("    group ", gi, " flags=0x", std::hex, group.groupFlags, std::dec,
                        " tris=", group.indices.size() / 3, " hits=", allHits,
                        " closestToFeetZ=", allZ, " gridHits=", gridHits, " gridClosestZ=", gridZ);
        }
    }

    // Doodads: everything the doodad floor would consider here.
    std::vector<const M2Placement*> candidates;
    gatherM2(glm::vec3(x - 2.0f, y - 2.0f, z - 6.0f), glm::vec3(x + 2.0f, y + 2.0f, z + 8.0f), candidates);
    int doodads = 0;
    for (const M2Placement* p : candidates) {
        if (++doodads > 12) { LOG_WARNING("  ... and more doodads"); break; }
        LOG_WARNING("  M2 instance ", p->id, " '", p->shape->name, "' tris=", p->shape->triCount,
                    " bounds z ", p->boundsMin.z, "..", p->boundsMax.z,
                    p->skipCollision ? "  skipCollision" : "", p->scale <= 0.001f ? "  zero scale" : "");
    }

    // A step to either side: a floor a third of a yard away at the height
    // stood at separates a gap in the mesh from a real ledge walked off.
    constexpr float kStep = 0.35f;
    const std::pair<float, float> offsets[] = {{0, 0}, {kStep, 0}, {-kStep, 0}, {0, kStep}, {0, -kStep}};
    const char* names[] = {"centre", "+x", "-x", "+y", "-y"};
    std::string line;
    for (size_t i = 0; i < 5; ++i) {
        const auto h = wmoFloor(Reach::everywhere(), x + offsets[i].first, y + offsets[i].second, z);
        line += names[i];
        line += h ? ("=" + std::to_string(*h)) : std::string("=none");
        line += "  ";
    }
    LOG_WARNING("  building floor a step aside: ", line);
    LOG_WARNING("=== ", buildings, " buildings, ", doodads, " doodads near ===");
}

CollisionWorld::QueryStats CollisionWorld::wmoQueryStats() const {
    return {wmoQueryMs_.load(std::memory_order_relaxed), wmoQueryCalls_.load(std::memory_order_relaxed)};
}

CollisionWorld::QueryStats CollisionWorld::m2QueryStats() const {
    return {m2QueryMs_.load(std::memory_order_relaxed), m2QueryCalls_.load(std::memory_order_relaxed)};
}

void CollisionWorld::resetQueryStats() {
    wmoQueryMs_ = 0.0;
    wmoQueryCalls_ = 0;
    m2QueryMs_ = 0.0;
    m2QueryCalls_ = 0;
}

uint64_t CollisionWorld::bytes() const {
    uint64_t total = sizeof(CollisionWorld);
    std::unordered_set<const void*> counted;
    for (const auto& [id, p] : m2_) {
        total += sizeof(p);
        if (counted.insert(p.shape.get()).second) total += p.shape->bytes();
    }
    for (const auto& [id, p] : wmo_) {
        total += sizeof(p) + p.groupBounds.capacity() * sizeof(p.groupBounds[0]);
        if (counted.insert(p.shape.get()).second) total += p.shape->bytes();
    }
    total += terrain_.size() * sizeof(TerrainTileHeights);
    for (const auto* grid : {&m2Grid_, &wmoGrid_}) {
        for (const auto& [cell, ids] : *grid) total += sizeof(cell) + ids.capacity() * sizeof(uint32_t);
    }
    return total;
}

}  // namespace wowee::collision
