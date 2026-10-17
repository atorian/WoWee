#include "rendering/liquid_scene.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/water_mask.hpp"
#include "rendering/water_surface_grid.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace wowee {
namespace rendering {

namespace {

struct Bounds2 {
    glm::vec2 min{std::numeric_limits<float>::max()};
    glm::vec2 max{std::numeric_limits<float>::lowest()};
    void add(const glm::vec3& p) {
        min = glm::min(min, glm::vec2(p));
        max = glm::max(max, glm::vec2(p));
    }
    [[nodiscard]] bool overlaps(const Bounds2& o, float margin) const {
        return min.x - margin <= o.max.x && o.min.x - margin <= max.x &&
               min.y - margin <= o.max.y && o.min.y - margin <= max.y;
    }
};

Bounds2 surfaceBounds(const LiquidSurface& s) {
    const glm::vec3 ex = s.stepX * static_cast<float>(s.width);
    const glm::vec3 ey = s.stepY * static_cast<float>(s.height);
    Bounds2 b;
    b.add(s.origin);
    b.add(s.origin + ex);
    b.add(s.origin + ey);
    b.add(s.origin + ex + ey);
    return b;
}

glm::vec3 cellPoint(const LiquidSurface& s, float gx, float gy) {
    return s.origin + s.stepX * gx + s.stepY * gy;
}

/// The height at a cell's centre, or the surface's floor when its heights
/// did not load - the same fallback the drawn mesh uses.
float cellHeight(const LiquidSurface& s, int cx, int cy) {
    const auto h = sampleGridHeight(s.heights, s.width, s.height,
                                    static_cast<float>(cx) + 0.5f, static_cast<float>(cy) + 0.5f);
    return h ? *h : s.minHeight;
}

float cellArea(const LiquidSurface& s) {
    return std::abs(s.stepX.x * s.stepY.y - s.stepX.y * s.stepY.x);
}

} // namespace

bool LiquidSurface::cellHasLiquid(int x, int y) const {
    return waterCellRendered(mask, wmoId, width, xOffset, yOffset, x, y);
}

LiquidScene::LiquidScene() = default;
LiquidScene::~LiquidScene() = default;

LiquidScene::Subscription& LiquidScene::Subscription::operator=(Subscription&& o) noexcept {
    if (this != &o) {
        reset();
        scene_ = o.scene_;
        listener_ = o.listener_;
        o.scene_ = nullptr;
        o.listener_ = nullptr;
    }
    return *this;
}

void LiquidScene::Subscription::reset() {
    if (scene_ && listener_) {
        auto& ls = scene_->listeners_;
        ls.erase(std::remove(ls.begin(), ls.end(), listener_), ls.end());
    }
    scene_ = nullptr;
    listener_ = nullptr;
}

void LiquidScene::setLiquidTypes(const pipeline::LiquidTypeTable* types) {
    types_ = types ? types : &basicTypes_;
    if (!surfaces_.empty()) {
        // What is loaded was classified by the table it came in under; the
        // renderer's copies were built from that. Said rather than half-fixed.
        LOG_WARNING("Water: liquid types replaced with ", surfaces_.size(),
                    " surfaces already loaded - they keep the kinds they were given until "
                    "they stream out");
    }
}

pipeline::LiquidBasic LiquidScene::basicOf(uint16_t liquidType) const {
    return types_->basicOf(liquidType);
}

LiquidScene::Subscription LiquidScene::subscribe(LiquidSceneListener& listener) {
    listeners_.push_back(&listener);
    for (const auto& s : surfaces_) listener.liquidSurfaceAdded(s);
    return Subscription(this, &listener);
}

// ==============================================================
// Building
// ==============================================================

std::size_t LiquidScene::addTerrainTile(const pipeline::ADTTerrain& terrain, int tileX, int tileY) {
    constexpr float TILE_SIZE = 33.33333f / 8.0f;

    // ── Pass 1: collect layers into merge groups keyed by {liquidType, roundedHeight} ──
    struct ChunkLayerInfo {
        int chunkX, chunkY;
        const pipeline::ADTTerrain::WaterLayer* layer;
    };

    struct MergeKey {
        uint16_t liquidType;
        int32_t roundedHeight;  // minHeight * 2, rounded to int
        bool operator==(const MergeKey& o) const {
            return liquidType == o.liquidType && roundedHeight == o.roundedHeight;
        }
    };

    struct MergeKeyHash {
        size_t operator()(const MergeKey& k) const {
            return std::hash<uint64_t>()((uint64_t(k.liquidType) << 32) | uint32_t(k.roundedHeight));
        }
    };

    std::unordered_map<MergeKey, std::vector<ChunkLayerInfo>, MergeKeyHash> mergeGroups;

    for (int chunkIdx = 0; chunkIdx < 256; chunkIdx++) {
        const auto& chunkWater = terrain.waterData[chunkIdx];
        if (!chunkWater.hasWater()) continue;

        int chunkX = chunkIdx % 16;
        int chunkY = chunkIdx / 16;

        for (const auto& layer : chunkWater.layers) {
            MergeKey key;
            key.liquidType = layer.liquidType;
            key.roundedHeight = static_cast<int32_t>(std::round(layer.minHeight * 2.0f));
            mergeGroups[key].push_back({.chunkX = chunkX, .chunkY = chunkY, .layer = &layer});
        }
    }

    // Tile origin = NW corner = chunk(0,0) position
    const auto& chunk00 = terrain.getChunk(0, 0);

    // Stormwind water lowering check
    bool isStormwindArea = (tileX >= 28 && tileX <= 50 && tileY >= 28 && tileY <= 52);
    float tileWorldX = 0, tileWorldY = 0;
    glm::vec2 moonwellPos2D(0.0f);
    if (isStormwindArea) {
        tileWorldX = (32.0f - tileX) * core::coords::TILE_SIZE;
        tileWorldY = (32.0f - tileY) * core::coords::TILE_SIZE;
        moonwellPos2D = glm::vec2(-8755.9f, 1108.9f);
    }

    std::size_t totalSurfaces = 0;

    // Merge threshold: groups with more than this many chunks get merged into
    // one tile-wide surface.  Small groups (shore, lakes) stay per-chunk so
    // their original mask / height data is preserved exactly.
    constexpr size_t MERGE_THRESHOLD = 4;

    // ── Pass 2: create surfaces ──
    for (auto& [key, chunkLayers] : mergeGroups) {

        // ── Small group → per-chunk surfaces (original code path) ──
        if (chunkLayers.size() <= MERGE_THRESHOLD) {
            for (const auto& info : chunkLayers) {
                const auto& layer = *info.layer;
                const auto& terrainChunk = terrain.getChunk(info.chunkX, info.chunkY);

                LiquidSurface surface;
                const glm::vec3 position(terrainChunk.position[0], terrainChunk.position[1],
                                         layer.minHeight);
                surface.origin = glm::vec3(
                    // Same axis convention as terrain_mesh.cpp vertices:
                    // MH2O rows (y) advance along render X, columns (x)
                    // along render Y.
                    position.x - (static_cast<float>(layer.y) * TILE_SIZE),
                    position.y - (static_cast<float>(layer.x) * TILE_SIZE),
                    layer.minHeight
                );
                // stepX walks the mesh grid's x (MH2O column → render Y),
                // stepY walks the grid's y (MH2O row → render X).
                surface.stepX = glm::vec3(0.0f, -TILE_SIZE, 0.0f);
                surface.stepY = glm::vec3(-TILE_SIZE, 0.0f, 0.0f);

                surface.minHeight = layer.minHeight;
                surface.maxHeight = layer.maxHeight;
                surface.liquidType = layer.liquidType;
                // Keep mask coordinates in the MH2O chunk's native x/y order;
                // only the world-space origin needs the terrain axis conversion.
                surface.xOffset = layer.x;
                surface.yOffset = layer.y;
                surface.width = layer.width;
                surface.height = layer.height;

                size_t numVertices = (layer.width + 1) * (layer.height + 1);
                bool useFlat = true;
                if (layer.heights.size() == numVertices) {
                    bool sane = true;
                    for (float h : layer.heights) {
                        if (!std::isfinite(h) || std::abs(h) > 50000.0f) { sane = false; break; }
                        if (h < layer.minHeight - 8.0f || h > layer.maxHeight + 8.0f) { sane = false; break; }
                    }
                    if (sane) { useFlat = false; surface.heights = layer.heights; }
                }
                if (useFlat) surface.heights.resize(numVertices, layer.minHeight);

                if (isStormwindArea && layer.minHeight > 94.0f) {
                    float distToMoonwell = glm::distance(glm::vec2(tileWorldX, tileWorldY), moonwellPos2D);
                    if (distToMoonwell > 300.0f) {
                        for (float& h : surface.heights) h -= 1.0f;
                        surface.minHeight -= 1.0f;
                        surface.maxHeight -= 1.0f;
                    }
                }

                surface.mask = layer.mask;
                surface.tileX = tileX;
                surface.tileY = tileY;

                addSurface(std::move(surface));
                totalSurfaces++;
            }
            continue;
        }

        // ── Large group → merged tile-wide surface ──
        LiquidSurface surface;

        float groupHeight = key.roundedHeight / 2.0f;

        surface.width = 128;
        surface.height = 128;
        surface.xOffset = 0;
        surface.yOffset = 0;
        surface.liquidType = key.liquidType;
        surface.tileX = tileX;
        surface.tileY = tileY;

        // Origin = chunk(0,0) position (NW corner of tile)
        surface.origin = glm::vec3(chunk00.position[0], chunk00.position[1], groupHeight);
        surface.stepX = glm::vec3(-TILE_SIZE, 0.0f, 0.0f);
        surface.stepY = glm::vec3(0.0f, -TILE_SIZE, 0.0f);

        surface.minHeight = groupHeight;
        surface.maxHeight = groupHeight;

        // Initialize height grid (129×129) with group height
        constexpr int MERGED_W = 128;
        const int gridW = MERGED_W + 1;  // 129
        const int gridH = MERGED_W + 1;
        surface.heights.resize(gridW * gridH, groupHeight);

        // Initialize mask (128×128 sub-tiles)
        // Mask uses LSB bit order: tileIndex = row * 128 + col
        const int maskBytes = (MERGED_W * MERGED_W + 7) / 8;
        // Ocean water (basicType 1) at sea level fills each chunk that declares
        // it, whatever its own bitmap says - an ocean layer's sub-rect is often
        // sparse and the gaps read as holes in open sea.
        //
        // Each chunk that declares it, not the whole tile. This used to seed
        // the entire 128×128 mask, on the reading that depth testing against
        // the terrain would hide the water wherever there was land. That holds
        // only for land *above* sea level. Somewhere the ground drops below it
        // the water is drawn over the top with nothing to occlude it, and a
        // chunk that declares no liquid at all got a surface anyway: the
        // Caverns of Time, a hole in a coastal tile in Tanaris, was roofed with
        // ocean. Any cave below sea level on an ocean tile had the same.
        bool isOcean = (basicOf(key.liquidType) == pipeline::LiquidBasic::Ocean) &&
                       (std::abs(groupHeight) < 1.0f);
        surface.mask.resize(maskBytes, 0x00);

        // ── Fill from each contributing chunk ──
        for (const auto& info : chunkLayers) {
            const auto& layer = *info.layer;

            // Merged grid offset for this chunk.
            // Grid x is the row axis (render X): chunk row (idx/16) and MH2O y.
            // Grid y is the column axis (render Y): chunk col (idx%16) and MH2O x.
            int baseGx = info.chunkY * 8;
            int baseGy = info.chunkX * 8;

            // Copy heights
            int layerGridW = layer.width + 1;
            size_t numVertices = static_cast<size_t>(layerGridW) * (layer.height + 1);
            bool useFlat = true;
            if (layer.heights.size() == numVertices) {
                bool sane = true;
                for (float h : layer.heights) {
                    if (!std::isfinite(h) || std::abs(h) > 50000.0f) { sane = false; break; }
                    if (h < layer.minHeight - 8.0f || h > layer.maxHeight + 8.0f) { sane = false; break; }
                }
                if (sane) useFlat = false;
            }

            for (int ly = 0; ly <= layer.height; ly++) {
                for (int lx = 0; lx <= layer.width; lx++) {
                    int mgx = baseGx + layer.y + ly;
                    int mgy = baseGy + layer.x + lx;
                    if (mgx >= gridW || mgy >= gridH) continue;

                    float h;
                    if (!useFlat) {
                        int layerIdx = ly * layerGridW + lx;
                        h = layer.heights[layerIdx];
                    } else {
                        h = layer.minHeight;
                    }

                    surface.heights[mgy * gridW + mgx] = h;
                    if (h < surface.minHeight) surface.minHeight = h;
                    if (h > surface.maxHeight) surface.maxHeight = h;
                }
            }

            // Which of this chunk's sixty-four sub-tiles hold liquid. The rule
            // lives in water_mask.hpp so it can be checked on its own; this
            // places the answer, in the chunk-row-to-render-X order the merged
            // grid uses.
            const uint64_t chunkMask = chunkWaterMask(
                layer.mask, layer.x, layer.y, layer.width, layer.height, isOcean);
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 8; col++) {
                    if (!(chunkMask & (uint64_t{1} << (row * 8 + col)))) continue;
                    const int mx = baseGx + row;
                    const int my = baseGy + col;
                    if (mx >= MERGED_W || my >= MERGED_W) continue;
                    const int mergedTileIdx = my * MERGED_W + mx;
                    surface.mask[mergedTileIdx / 8] |=
                        static_cast<uint8_t>(1 << (mergedTileIdx % 8));
                }
            }
        }

        // Stormwind water lowering
        if (isStormwindArea && surface.minHeight > 94.0f) {
            float distToMoonwell = glm::distance(glm::vec2(tileWorldX, tileWorldY), moonwellPos2D);
            if (distToMoonwell > 300.0f) {
                for (float& h : surface.heights) h -= 1.0f;
                surface.minHeight -= 1.0f;
                surface.maxHeight -= 1.0f;
            }
        }

        addSurface(std::move(surface));
        totalSurfaces++;
    }

    if (totalSurfaces > 0) {
        LOG_INFO("Water: Loaded ", totalSurfaces, " surfaces from tile [", tileX, ",", tileY,
                 "] (", mergeGroups.size(), " groups), total surfaces: ", surfaces_.size());
    }
    return totalSurfaces;
}

bool LiquidScene::addWmoLiquid(const pipeline::WMOLiquid& liquid, const glm::mat4& modelMatrix,
                               uint32_t wmoId) {
    if (!liquid.hasLiquid()) return false;

    // A liquid the data describes and this cannot lay down is the building's
    // pool or canal missing from the world, and from swimming. Said once per
    // instance, with what was wrong with it.
    auto reject = [&](const char* why) {
        if (rejectedWmoSaid_.insert(wmoId).second) {
            LOG_WARNING("Water: WMO instance ", wmoId, " liquid not placed (", why, "; ",
                        liquid.xTiles, "x", liquid.yTiles, " tiles, ", liquid.xVerts, "x",
                        liquid.yVerts, " vertices, liquid type ", liquid.liquidType,
                        ") - the building is drawn dry and cannot be swum in there");
        }
        return false;
    };

    if (liquid.xTiles == 0 || liquid.yTiles == 0) return reject("no tiles");
    if (liquid.xVerts < 2 || liquid.yVerts < 2) return reject("fewer than two vertices a side");
    if (liquid.xTiles != liquid.xVerts - 1 || liquid.yTiles != liquid.yVerts - 1)
        return reject("tile and vertex counts disagree");
    if (liquid.xTiles > 64 || liquid.yTiles > 64) return reject("more than 64 tiles a side");

    LiquidSurface surface;
    surface.tileX = -1;
    surface.tileY = -1;
    surface.wmoId = wmoId;
    surface.liquidType = liquid.liquidType;
    surface.xOffset = 0;
    surface.yOffset = 0;
    surface.width = static_cast<uint8_t>(std::min<uint32_t>(255, liquid.xTiles));
    surface.height = static_cast<uint8_t>(std::min<uint32_t>(255, liquid.yTiles));

    constexpr float WMO_LIQUID_TILE_SIZE = 4.1666625f;
    const glm::vec3 localBase(liquid.basePosition.x, liquid.basePosition.y, liquid.basePosition.z);
    const glm::vec3 localStepX(WMO_LIQUID_TILE_SIZE, 0.0f, 0.0f);
    const glm::vec3 localStepY(0.0f, WMO_LIQUID_TILE_SIZE, 0.0f);

    surface.origin = glm::vec3(modelMatrix * glm::vec4(localBase, 1.0f));
    surface.stepX = glm::vec3(modelMatrix * glm::vec4(localStepX, 0.0f));
    surface.stepY = glm::vec3(modelMatrix * glm::vec4(localStepY, 0.0f));

    float stepXLen = glm::length(surface.stepX);
    float stepYLen = glm::length(surface.stepY);
    glm::vec3 planeN = glm::cross(surface.stepX, surface.stepY);
    float planeNLenSq = glm::dot(planeN, planeN);
    float nz = (planeNLenSq > 1e-8f) ? std::abs(planeN.z * glm::inversesqrt(planeNLenSq)) : 0.0f;
    float spanX = stepXLen * static_cast<float>(surface.width);
    float spanY = stepYLen * static_cast<float>(surface.height);
    if (stepXLen < 0.2f || stepXLen > 12.0f ||
        stepYLen < 0.2f || stepYLen > 12.0f ||
        nz < 0.60f || spanX > 450.0f || spanY > 450.0f) {
        return reject("placed at a scale or tilt no liquid has");
    }

    const int gridWidth = static_cast<int>(surface.width) + 1;
    const int gridHeight = static_cast<int>(surface.height) + 1;
    const int vertexCount = gridWidth * gridHeight;

    // WMO liquid base heights sit ~2 units above the visual waterline.
    constexpr float WMO_WATER_Z_OFFSET = -1.0f;
    float adjustedZ = surface.origin.z + WMO_WATER_Z_OFFSET;
    surface.heights.assign(vertexCount, adjustedZ);
    surface.minHeight = adjustedZ;
    surface.maxHeight = adjustedZ;
    surface.origin.z = adjustedZ;

    if (surface.origin.z > 2000.0f || surface.origin.z < -500.0f)
        return reject("placed outside the world's height range");

    // Build tile mask from MLIQ flags
    size_t tileCount = static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height);
    size_t maskBytes = (tileCount + 7) / 8;
    surface.mask.assign(maskBytes, 0x00);
    size_t activeTiles = 0;
    for (size_t t = 0; t < tileCount; t++) {
        bool hasLiquid = true;
        int tx = static_cast<int>(t) % surface.width;
        int ty = static_cast<int>(t) / surface.width;

        // Standard WoW check: low nibble 0x0F = "don't render"
        if (t < liquid.flags.size()) {
            if ((liquid.flags[t] & 0x0F) == 0x0F) {
                hasLiquid = false;
            }
        }
        // Suppress water tiles that extend into enclosed WMO areas
        // (e.g. Stormwind barracks stairway where canal water pokes through)
        // Render coords: x=wowY(west), y=wowX(north)
        if (hasLiquid) {
            glm::vec3 tileWorld = surface.origin +
                surface.stepX * (static_cast<float>(tx) + 0.5f) +
                surface.stepY * (static_cast<float>(ty) + 0.5f);
            // Stormwind Barracks / Stockade stairway:
            // Stockade entrance at approximately render (-8768, 848)
            if (tileWorld.x > -8790.0f && tileWorld.x < -8735.0f &&
                tileWorld.y > 828.0f && tileWorld.y < 878.0f) {
                hasLiquid = false;
            }
        }
        if (hasLiquid) {
            size_t byteIdx = t / 8;
            size_t bitIdx = t % 8;
            surface.mask[byteIdx] |= (1 << bitIdx);
            activeTiles++;
        }
    }

    LOG_DEBUG("WMO water: origin=(", surface.origin.x, ",", surface.origin.y, ",", surface.origin.z,
              ") tiles=", static_cast<int>(surface.width), "x", static_cast<int>(surface.height),
              " active=", activeTiles, "/", tileCount, " wmoId=", wmoId);

    // Every tile flagged dry is a group whose liquid is all "don't render" -
    // the data's own way of saying there is none, not a failure.
    if (activeTiles == 0) return false;
    addSurface(std::move(surface));
    return true;
}

void LiquidScene::addSurface(LiquidSurface&& surface) {
    surface.id = nextId_++;
    surface.basic = basicOf(surface.liquidType);
    surfaces_.push_back(std::move(surface));
    const LiquidSurface& stored = surfaces_.back();
    buildPatches(stored);
    joinPatches(stored);
    bodiesDirty_ = true;
    for (auto* l : listeners_) l->liquidSurfaceAdded(stored);
}

template <class Pred>
void LiquidScene::removeSurfacesIf(Pred pred) {
    std::vector<LiquidSurfaceId> gone;
    for (const auto& s : surfaces_) {
        if (pred(s)) gone.push_back(s.id);
    }
    if (gone.empty()) return;
    for (LiquidSurfaceId id : gone) {
        for (auto* l : listeners_) l->liquidSurfaceRemoved(id);
        dropPatches(id);
    }
    surfaces_.erase(std::remove_if(surfaces_.begin(), surfaces_.end(), pred), surfaces_.end());
    bodiesDirty_ = true;
}

void LiquidScene::removeTile(int tileX, int tileY) {
    removeSurfacesIf([&](const LiquidSurface& s) {
        return s.wmoId == 0 && s.tileX == tileX && s.tileY == tileY;
    });
}

void LiquidScene::removeWmo(uint32_t wmoId) {
    if (wmoId == 0) return;
    removeSurfacesIf([&](const LiquidSurface& s) { return s.wmoId == wmoId; });
    rejectedWmoSaid_.erase(wmoId);
}

void LiquidScene::clear() {
    removeSurfacesIf([](const LiquidSurface&) { return true; });
    rejectedWmoSaid_.clear();
}

// ==============================================================
// Patches and bodies
// ==============================================================

void LiquidScene::buildPatches(const LiquidSurface& s) {
    SurfacePatches& sp = surfacePatches_[s.id];
    const int w = s.width;
    const int h = s.height;
    sp.cellPatch.assign(static_cast<size_t>(w) * h, -1);
    const float area = cellArea(s);

    std::deque<int> open;
    for (int start = 0; start < w * h; ++start) {
        if (sp.cellPatch[start] != -1 || !s.cellHasLiquid(start % w, start / w)) continue;

        const uint32_t index = sp.patchCount++;
        Patch patch;
        patch.surface = s.id;
        patch.minHeight = std::numeric_limits<float>::max();
        patch.maxHeight = std::numeric_limits<float>::lowest();
        Bounds2 bounds;

        sp.cellPatch[start] = static_cast<int32_t>(index);
        open.push_back(start);
        while (!open.empty()) {
            const int c = open.front();
            open.pop_front();
            const int cx = c % w;
            const int cy = c / w;
            patch.area += area;
            bounds.add(cellPoint(s, static_cast<float>(cx), static_cast<float>(cy)));
            bounds.add(cellPoint(s, static_cast<float>(cx + 1), static_cast<float>(cy + 1)));
            const float ch = cellHeight(s, cx, cy);
            patch.minHeight = std::min(patch.minHeight, ch);
            patch.maxHeight = std::max(patch.maxHeight, ch);

            constexpr int kDx[4] = {1, -1, 0, 0};
            constexpr int kDy[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const int nx = cx + kDx[d];
                const int ny = cy + kDy[d];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const int n = ny * w + nx;
                if (sp.cellPatch[n] != -1 || !s.cellHasLiquid(nx, ny)) continue;
                // Within one surface a step of more than a river's slope is a
                // drop from one pool to another.
                if (std::abs(cellHeight(s, nx, ny) - ch) > kMaxJoinStep) continue;
                sp.cellPatch[n] = static_cast<int32_t>(index);
                open.push_back(n);
            }
        }
        patch.boundsMin = bounds.min;
        patch.boundsMax = bounds.max;
        patches_.emplace(patchKey(s.id, index), std::move(patch));
    }
}

void LiquidScene::joinPatches(const LiquidSurface& s) {
    const SurfacePatches& own = surfacePatches_.at(s.id);
    if (own.patchCount == 0) return;

    const Bounds2 bounds = surfaceBounds(s);
    const float margin = std::max(glm::length(s.stepX), glm::length(s.stepY));
    std::vector<const LiquidSurface*> candidates;
    for (const auto& other : surfaces_) {
        if (other.id == s.id || other.basicType() != s.basicType()) continue;
        if (!surfaceBounds(other).overlaps(bounds, margin)) continue;
        candidates.push_back(&other);
    }
    if (candidates.empty()) return;

    const int w = s.width;
    const int h = s.height;
    constexpr int kDx[4] = {1, -1, 0, 0};
    constexpr int kDy[4] = {0, 0, 1, -1};
    for (int cy = 0; cy < h; ++cy) {
        for (int cx = 0; cx < w; ++cx) {
            const int32_t mine = own.cellPatch[static_cast<size_t>(cy) * w + cx];
            if (mine < 0) continue;
            const float myHeight = cellHeight(s, cx, cy);
            for (int d = 0; d < 4; ++d) {
                const int nx = cx + kDx[d];
                const int ny = cy + kDy[d];
                // A neighbour inside this surface that holds liquid is this
                // surface's own business; only edges look outward.
                if (nx >= 0 && ny >= 0 && nx < w && ny < h && s.cellHasLiquid(nx, ny)) continue;

                const glm::vec3 p = cellPoint(s, static_cast<float>(nx) + 0.5f,
                                              static_cast<float>(ny) + 0.5f);
                for (const LiquidSurface* other : candidates) {
                    const auto grid = surfaceGridPosition(other->origin, other->stepX, other->stepY,
                                                          other->width, other->height, p.x, p.y);
                    if (!grid) continue;
                    const int ox = std::min(static_cast<int>(grid->x), static_cast<int>(other->width) - 1);
                    const int oy = std::min(static_cast<int>(grid->y), static_cast<int>(other->height) - 1);
                    const auto it = surfacePatches_.find(other->id);
                    if (it == surfacePatches_.end()) continue;
                    const int32_t theirs = it->second.cellPatch[static_cast<size_t>(oy) * other->width + ox];
                    if (theirs < 0) continue;
                    if (std::abs(cellHeight(*other, ox, oy) - myHeight) > kMaxJoinStep) continue;

                    const uint64_t a = patchKey(s.id, static_cast<uint32_t>(mine));
                    const uint64_t b = patchKey(other->id, static_cast<uint32_t>(theirs));
                    patches_.at(a).neighbours.insert(b);
                    patches_.at(b).neighbours.insert(a);
                }
            }
        }
    }
}

void LiquidScene::dropPatches(LiquidSurfaceId id) {
    const auto sp = surfacePatches_.find(id);
    if (sp == surfacePatches_.end()) return;
    for (uint32_t i = 0; i < sp->second.patchCount; ++i) {
        const uint64_t key = patchKey(id, i);
        const auto it = patches_.find(key);
        if (it == patches_.end()) continue;
        for (uint64_t n : it->second.neighbours) {
            const auto other = patches_.find(n);
            if (other != patches_.end()) other->second.neighbours.erase(key);
        }
        patches_.erase(it);
    }
    surfacePatches_.erase(sp);
}

void LiquidScene::findBodies() const {
    if (!bodiesDirty_) return;
    bodiesDirty_ = false;
    bodyOfPatch_.clear();
    bodies_.clear();

    // Patch keys in order, so each body is named after its oldest patch.
    std::vector<uint64_t> keys;
    keys.reserve(patches_.size());
    for (const auto& [key, patch] : patches_) keys.push_back(key);
    std::sort(keys.begin(), keys.end());

    std::vector<uint64_t> open;
    std::unordered_set<LiquidSurfaceId> spanned;
    for (uint64_t start : keys) {
        if (bodyOfPatch_.count(start)) continue;
        const LiquidBodyId id = start;
        LiquidBody body;
        body.id = id;
        body.boundsMin = glm::vec2(std::numeric_limits<float>::max());
        body.boundsMax = glm::vec2(std::numeric_limits<float>::lowest());
        body.minHeight = std::numeric_limits<float>::max();
        body.maxHeight = std::numeric_limits<float>::lowest();
        spanned.clear();

        bodyOfPatch_[start] = id;
        open.push_back(start);
        while (!open.empty()) {
            const uint64_t key = open.back();
            open.pop_back();
            const Patch& p = patches_.at(key);
            body.area += p.area;
            body.boundsMin = glm::min(body.boundsMin, p.boundsMin);
            body.boundsMax = glm::max(body.boundsMax, p.boundsMax);
            body.minHeight = std::min(body.minHeight, p.minHeight);
            body.maxHeight = std::max(body.maxHeight, p.maxHeight);
            spanned.insert(p.surface);
            for (uint64_t n : p.neighbours) {
                if (bodyOfPatch_.emplace(n, id).second) open.push_back(n);
            }
        }
        if (const LiquidSurface* s = find(static_cast<LiquidSurfaceId>(start >> 32))) {
            body.basicType = s->basicType();
        }
        for (LiquidSurfaceId sid : spanned) {
            if (const LiquidSurface* s = find(sid); s && s->wmoId != 0) body.inWmo = true;
        }
        body.surfaceCount = static_cast<uint32_t>(spanned.size());
        bodies_.emplace(id, body);
    }
}

const LiquidSurface* LiquidScene::find(LiquidSurfaceId id) const {
    for (const auto& s : surfaces_) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::optional<LiquidScene::CellHit> LiquidScene::highestCellAt(float x, float y) const {
    std::optional<CellHit> best;
    float bestHeight = 0.0f;
    for (const auto& s : surfaces_) {
        const auto grid = surfaceGridPosition(s.origin, s.stepX, s.stepY, s.width, s.height, x, y);
        if (!grid) continue;
        const int ix = std::min(static_cast<int>(grid->x), static_cast<int>(s.width) - 1);
        const int iy = std::min(static_cast<int>(grid->y), static_cast<int>(s.height) - 1);
        if (!s.cellHasLiquid(ix, iy)) continue;
        const auto h = sampleGridHeight(s.heights, s.width, s.height, grid->x, grid->y);
        if (!h) continue;
        if (!best || *h > bestHeight) {
            best = CellHit{.surface = &s, .cellX = ix, .cellY = iy};
            bestHeight = *h;
        }
    }
    return best;
}

std::optional<LiquidBody> LiquidScene::bodyAt(float x, float y) const {
    const auto hit = highestCellAt(x, y);
    if (!hit) return std::nullopt;
    const auto sp = surfacePatches_.find(hit->surface->id);
    if (sp == surfacePatches_.end()) return std::nullopt;
    const int32_t patch = sp->second.cellPatch[static_cast<size_t>(hit->cellY) * hit->surface->width +
                                               hit->cellX];
    if (patch < 0) return std::nullopt;
    findBodies();
    const auto b = bodyOfPatch_.find(patchKey(hit->surface->id, static_cast<uint32_t>(patch)));
    if (b == bodyOfPatch_.end()) return std::nullopt;
    return bodies_.at(b->second);
}

std::vector<LiquidBody> LiquidScene::bodies() const {
    findBodies();
    std::vector<LiquidBody> out;
    out.reserve(bodies_.size());
    for (const auto& [id, body] : bodies_) out.push_back(body);
    std::sort(out.begin(), out.end(),
              [](const LiquidBody& a, const LiquidBody& b) { return a.id < b.id; });
    return out;
}

// ==============================================================
// Queries
// ==============================================================

std::optional<float> LiquidScene::heightAt(float x, float y) const {
    std::optional<float> best;
    for (const auto& s : surfaces_) {
        const auto grid = surfaceGridPosition(s.origin, s.stepX, s.stepY, s.width, s.height, x, y);
        if (!grid) continue;
        const int ix = std::min(static_cast<int>(grid->x), static_cast<int>(s.width) - 1);
        const int iy = std::min(static_cast<int>(grid->y), static_cast<int>(s.height) - 1);
        if (!s.cellHasLiquid(ix, iy)) continue;
        const auto h = sampleGridHeight(s.heights, s.width, s.height, grid->x, grid->y);
        if (!h) continue;
        if (!best || *h > *best) best = *h;
    }
    return best;
}

std::optional<float> LiquidScene::nearestHeightAt(float x, float y, float z, float maxAbove) const {
    std::optional<float> best;
    float bestDist = 1e9f;
    for (const auto& s : surfaces_) {
        const auto grid = surfaceGridPosition(s.origin, s.stepX, s.stepY, s.width, s.height, x, y);
        if (!grid) continue;
        const int ix = std::min(static_cast<int>(grid->x), static_cast<int>(s.width) - 1);
        const int iy = std::min(static_cast<int>(grid->y), static_cast<int>(s.height) - 1);
        if (!s.cellHasLiquid(ix, iy)) continue;
        const auto sampled = sampleGridHeight(s.heights, s.width, s.height, grid->x, grid->y);
        if (!sampled) continue;
        const float h = *sampled;
        if (h < z - 2.0f) continue;       // below the query
        if (h > z + maxAbove) continue;   // far above it
        const float dist = std::abs(h - z);
        if (!best || dist < bestDist) {
            best = h;
            bestDist = dist;
        }
    }
    return best;
}

std::optional<uint16_t> LiquidScene::typeAt(float x, float y) const {
    std::optional<float> bestHeight;
    std::optional<uint16_t> bestType;
    for (const auto& s : surfaces_) {
        const auto grid = surfaceGridPosition(s.origin, s.stepX, s.stepY, s.width, s.height, x, y);
        if (!grid) continue;
        const int ix = std::min(static_cast<int>(grid->x), static_cast<int>(s.width) - 1);
        const int iy = std::min(static_cast<int>(grid->y), static_cast<int>(s.height) - 1);
        if (!s.cellHasLiquid(ix, iy)) continue;
        if (!bestHeight || s.minHeight > *bestHeight) {
            bestHeight = s.minHeight;
            bestType = s.liquidType;
        }
    }
    return bestType;
}

std::optional<pipeline::LiquidBasic> LiquidScene::basicTypeAt(float x, float y) const {
    const auto type = typeAt(x, y);
    if (!type) return std::nullopt;
    return basicOf(*type);
}

bool LiquidScene::isWmoLiquidAt(float x, float y) const {
    for (const auto& s : surfaces_) {
        if (s.wmoId == 0) continue;
        if (surfaceGridPosition(s.origin, s.stepX, s.stepY, s.width, s.height, x, y)) return true;
    }
    return false;
}

std::optional<float> LiquidScene::dominantWaterHeight(const glm::vec3& cameraPos) const {
    float bestDist = std::numeric_limits<float>::max();
    std::optional<float> best;
    for (const auto& s : surfaces_) {
        // Only water and ocean are mirrored; magma and slime are not.
        if (s.basicType() >= 2) continue;
        const glm::vec3 center = s.origin +
            s.stepX * (static_cast<float>(s.width) * 0.5f) +
            s.stepY * (static_cast<float>(s.height) * 0.5f);
        const float dx = cameraPos.x - center.x;
        const float dy = cameraPos.y - center.y;
        const float dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            best = s.minHeight;
        }
    }
    return best;
}

} // namespace rendering
} // namespace wowee
