#pragma once
#include "rendering/terrain_placement_refs.hpp"

#include <functional>
#include "core/cache_stats.hpp"
#include "core/job_group.hpp"

#include "collision/terrain_heights.hpp"
#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/detail_doodads.hpp"
#include "rendering/detail_doodad_renderer.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/normal_map_encoding.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <list>
#include <vector>
#include <condition_variable>
#include <deque>
#include <glm/glm.hpp>
#include <array>

namespace wowee {

namespace pipeline { class AssetManager; class GameTables; }
namespace collision { class CollisionWorld; }
namespace rendering { class TerrainRenderer; class Camera; class LiquidScene; class M2Renderer; class WMORenderer; struct WmoModelPrep; }

namespace rendering {

/**
 * Terrain tile coordinates
 */
struct TileCoord {
    int x;
    int y;

    bool operator==(const TileCoord& other) const {
        return x == other.x && y == other.y;
    }

    struct Hash {
        size_t operator()(const TileCoord& coord) const {
            return std::hash<int>()(coord.x) ^ (std::hash<int>()(coord.y) << 1);
        }
    };
};

/**
 * Loaded terrain tile data
 */
enum class TileDetail { Detailed, Landmarks };

struct TerrainTile {
    TileDetail detail = TileDetail::Detailed;
    TileCoord coord;
    /// The parsed tile, which height, hole, area and texture queries read.
    /// Not the mesh built from it: that is on the GPU once the tile commits,
    /// nothing reads it after, and it was a gigabyte across a city's tiles.
    pipeline::ADTTerrain terrain;
    bool loaded = false;

    // Tile bounds in world coordinates
    float minX, minY, maxX, maxY;

    // Instance IDs for cleanup on unload
    std::vector<uint32_t> wmoInstanceIds;
    std::vector<uint32_t> wmoUniqueIds;  // For WMO dedup cleanup on unload
    std::vector<uint32_t> m2InstanceIds;
    std::vector<uint32_t> doodadUniqueIds;  // For dedup cleanup on unload
};

/**
 * Pre-processed tile data ready for GPU upload (produced by background thread)
 */
struct PendingTile {
    TileDetail detail = TileDetail::Detailed;
    core::JobGroup::Token token;
    TileCoord coord;
    pipeline::ADTTerrain terrain;
    pipeline::TerrainMesh mesh;
    /// Its ground for the collision world, handed over at the commit.
    std::shared_ptr<const collision::TerrainTileHeights> heights;

    /// Buildings this tile names that will not arrive, and why: told to the
    /// movement world when the tile is committed, on the main thread.
    std::vector<std::pair<std::string, std::string>> unavailableBuildings;

    // Pre-loaded M2 data
    struct M2Ready {
        uint32_t modelId;
        pipeline::M2Model model;
        std::string path;
    };
    std::vector<M2Ready> m2Models;

    // M2 instance placement data (references modelId from m2Models)
    struct M2Placement {
        uint32_t modelId;
        uint32_t uniqueId;
        glm::vec3 position;
        glm::vec3 rotation;
        float scale;
    };
    std::vector<M2Placement> m2Placements;

    /// Models of the detail doodads the tile's ground effects plant, read on
    /// the worker for DetailDoodadRenderer; `failed` when the model would not
    /// read, so the clutter that plants it does not wait for it.
    struct DetailModel {
        uint32_t doodadId = 0;
        bool failed = false;
        pipeline::DetailDoodadModel model;
        std::vector<std::pair<std::string, pipeline::BLPImage>> images;
    };
    std::vector<DetailModel> detailModels;

    // Pre-loaded WMO data
    struct WMOReady {
        uint32_t modelId;
        uint32_t uniqueId;
        pipeline::WMOModel model;
        /// Its CPU-side upload work, done here on the worker
        /// (WMORenderer::prepareModel); finalizing it is uploads and materials.
        std::shared_ptr<WmoModelPrep> prep;
        glm::vec3 position;
        glm::vec3 rotation;
        float scale = 1.0f;
        /// MODF NameSetID: which WMOAreaTable rows name the placement.
        uint16_t nameSet = 0;
    };
    std::vector<WMOReady> wmoModels;

    // WMO doodad M2 models (M2s placed inside WMOs)
    struct WMODoodadReady {
        size_t ownerPlacement = 0;
        uint32_t modelId;
        pipeline::M2Model model;
        glm::vec3 worldPosition;   // For frustum culling
        glm::mat4 modelMatrix;     // Pre-computed world transform
        // The doodad's light when an interior group holds it
        std::optional<InteriorLight> interiorLight;
    };
    std::vector<WMODoodadReady> wmoDoodads;

    // Ambient sound emitters (detected from doodads)
    struct AmbientEmitter {
        glm::vec3 position;
        uint32_t type;  // Maps to AmbientSoundManager::AmbientType
    };
    std::vector<AmbientEmitter> ambientEmitters;

    // Pre-loaded terrain texture BLP data (loaded on background thread to avoid
    // blocking file I/O on the main thread during finalizeTile)
    std::unordered_map<std::string, pipeline::BLPImage> preloadedTextures;
    // Their relief (TerrainRenderer::deriveNormalMap), keyed alike; none for
    // a texture whose relief was already in the cache.
    std::unordered_map<std::string, EncodedNormalMap> preloadedTerrainNormalMaps;

    // Pre-decoded M2 model textures (decoded on background thread)
    std::unordered_map<std::string, pipeline::BLPImage> preloadedM2Textures;

    // Pre-decoded WMO textures (decoded on background thread)
    std::unordered_map<std::string, pipeline::BLPImage> preloadedWMOTextures;
    // Normal/height maps for those WMO textures, derived and encoded with
    // their mips. Keeping this work on terrain workers avoids multi-second
    // main-thread backfill stalls.
    std::unordered_map<std::string, EncodedNormalMap> preloadedWMONormalMaps;
};

/**
 * Phases for incremental tile finalization (one bounded unit of work per call)
 */
enum class FinalizationPhase {
    TERRAIN,        // Upload terrain mesh + textures + water
    M2_MODELS,      // Upload ONE M2 model per call
    M2_INSTANCES,   // Create all M2 instances (lightweight struct allocation)
    WMO_MODELS,     // Upload ONE WMO model per call
    WMO_INSTANCES,  // Create all WMO instances + load WMO liquids
    WMO_DOODADS,    // Upload ONE WMO doodad M2 per call
    WATER,          // Generate water ambient emitters
    AMBIENT,        // Register ambient emitters + commit tile
    DONE            // Fully finalized
};

/**
 * In-progress tile finalization state - tracks progress across frames
 */
struct FinalizingTile {
    std::shared_ptr<PendingTile> pending;
    FinalizationPhase phase = FinalizationPhase::TERRAIN;

    // Progress indices within current phase
    size_t m2ModelIndex     = 0;   // Next M2 model to upload
    size_t m2InstanceIndex  = 0;   // Next M2 placement to instantiate
    size_t wmoModelIndex    = 0;   // Next WMO model to upload
    size_t wmoInstanceIndex = 0;   // Next WMO placement to instantiate
    size_t wmoDoodadIndex   = 0;   // Next WMO doodad to upload
    size_t wmoLiquidGroupIndex = 0; // Next liquid group within current WMO instance
    // The current placement's instance exists and its liquid is being laid
    // down over several steps. Without it a resumed placement looked like a
    // duplicate of itself - it had been marked placed on the first step - and
    // every liquid group after the first few was skipped.
    bool wmoInstanceOpen = false;
    std::vector<uint32_t> wmoPlacementInstances;

    // Incremental terrain upload state (splits TERRAIN phase across frames)
    bool terrainPreloaded = false;  // True after preloaded textures uploaded
    int terrainChunkNext = 0;       // Next chunk index to upload (0-255, row-major)
    bool terrainMeshDone = false;   // True when all chunks uploaded
    bool terrainWaterDone = false;  // True once the tile's water is in
    // The tile's textures, uploaded a few at a time rather than all at once:
    // the whole set in one call took 35-41 ms on Northrend tiles.
    std::vector<std::string> terrainTexKeys;
    size_t terrainTexNext = 0;
    bool terrainTexKeysCollected = false;

    // Accumulated results (built up across phases)
    std::vector<uint32_t> m2InstanceIds;
    std::vector<uint32_t> wmoInstanceIds;
    std::vector<uint32_t> tileUniqueIds;
    std::vector<uint32_t> tileWmoUniqueIds;
    std::unordered_set<uint32_t> uploadedM2ModelIds;
};

/**
 * Terrain manager for multi-tile terrain streaming
 *
 * Handles loading and unloading terrain tiles based on camera position
 */
class TerrainManager {
public:
    TerrainManager();
    ~TerrainManager();
    void setJobToken(core::JobGroup::Token token) { mapToken_ = std::move(token); }

    /**
     * Initialize terrain manager
     * @param assetManager Asset manager for loading files
     * @param terrainRenderer Terrain renderer for GPU upload
     * @param tables The expansion's tables (ground clutter)
     */
    bool initialize(pipeline::AssetManager* assetManager, TerrainRenderer* terrainRenderer,
                    const pipeline::GameTables& tables);

    /**
     * Update terrain streaming based on camera position
     * @param camera Current camera
     * @param deltaTime Time since last update
     */
    void update(const Camera& camera, float deltaTime);

    /**
     * Set map name
     * @param mapName Map name (e.g., "Azeroth", "Kalimdor")
     */
    void setMapName(const std::string& mapName) { this->mapName = mapName; }
    const std::string& getMapName() const { return mapName; }
    [[nodiscard]] bool isCustomZone() const { return isCustomZone_; }
    void setCustomZone(bool custom) { isCustomZone_ = custom; }


    /**
     * Enqueue a tile for async loading (returns false if previously failed).
     */
    bool enqueueTile(int x, int y);

    /**
     * Unload a tile
     * @param x Tile X coordinate
     * @param y Tile Y coordinate
     */
    void unloadTile(int x, int y);

    void stopWorkers();  // Stop worker threads without restarting (for shutdown)

    /**
     * Precache a set of tiles (for taxi routes, etc.)
     * @param tiles Vector of (x, y) tile coordinates to preload
     */
    void precacheTiles(const std::vector<std::pair<int, int>>& tiles);

    /**
     * Set streaming parameters
     */
    void setLoadRadius(int radius) { if(loadRadius!=radius) streamingPolicyDirty_=true; loadRadius = radius; }
    void setLandmarkRadius(int radius) { if(landmarkRadius_!=radius) streamingPolicyDirty_=true; landmarkRadius_ = radius; }
    void setUnloadRadius(int radius) { unloadRadius = radius; }
    void setStreamingEnabled(bool enabled) { streamingEnabled = enabled; }
    bool isStreamingEnabled() const { return streamingEnabled; }
    void setUpdateInterval(float seconds) { updateInterval = seconds; }
    void setTaxiStreamingMode(bool enabled) { taxiStreamingMode_ = enabled; }
    /// GroundEffectTexture and GroundEffectDoodad: what each texture layer grows.
    [[nodiscard]] const pipeline::GroundEffectTables& groundEffects() const { return groundEffects_; }
    /// Where the detail doodads' models go as the tiles that plant them are
    /// committed. Unset, the workers do not read them.
    void setDetailDoodadRenderer(DetailDoodadRenderer* renderer) { detailDoodadRenderer_ = renderer; }
    /// The chunks of the loaded detailed tiles whose centre is within
    /// `radius` of `center` across the ground: the ground clutter's candidates.
    void collectDetailChunks(const glm::vec2& center, float radius,
                             std::vector<DetailDoodadRenderer::NearbyChunk>& out) const;
    /// Where the tiles' and buildings' liquid goes as they stream in and out.
    void setLiquidScene(LiquidScene* scene) { liquidScene = scene; }
    /// Where each tile's ground goes as it is committed and leaves as it is
    /// unloaded - and every tile still loaded when this manager goes.
    void setCollisionWorld(collision::CollisionWorld* world) { collision_ = world; }
    void setM2Renderer(M2Renderer* renderer) { m2Renderer = renderer; }
    /// Also fixes how the workers encode the normal maps they prepare for it.
    void setWMORenderer(WMORenderer* renderer);
    /// Where a committed tile's sound emitters go - fires, torches, fountains,
    /// water - as a position and AmbientSoundManager's type code. Unset, the
    /// emitters are not gathered at all.
    using AmbientEmitterSink = std::function<void(const glm::vec3& position, uint32_t type)>;
    void setAmbientEmitterSink(AmbientEmitterSink sink) { ambientEmitterSink_ = std::move(sink); }

    /**
     * The chunk under a world position, plus the offsets within it: what the
     * renderer's own lookups of the parsed tile (textures, clutter) read.
     * The world's height, hole and area queries are CollisionWorld's, over
     * the same search.
     */
    const pipeline::MapChunk* findChunkAt(float glX, float glY,
                                          float& fracX, float& fracY,
                                          const TerrainTile** outTile = nullptr) const;


    /**
     * Get dominant terrain texture name at a GL position.
     * Returns empty if terrain is not loaded at that position.
     */
    [[nodiscard]] std::optional<std::string> getDominantTextureAt(float glX, float glY) const;

    /**
     * Get statistics
     */
    [[nodiscard]] int getLoadedTileCount() const { return static_cast<int>(loadedTiles.size()); }
    /// CPU memory the loaded tiles hold: the parsed ADT data.
    struct TileMemory {
        uint64_t terrainBytes = 0;
    };
    [[nodiscard]] TileMemory tileMemory() const;
    [[nodiscard]] int getPendingTileCount() const {
        std::lock_guard<std::mutex> lock(queueMutex);
        return static_cast<int>(pendingTiles.size());
    }
    [[nodiscard]] int getReadyQueueCount() const {
        std::lock_guard<std::mutex> lock(queueMutex);
        return static_cast<int>(readyQueue.size());
    }
    /** Total unfinished tiles (worker threads + ready queue + finalizing) */
    [[nodiscard]] int getRemainingTileCount() const {
        std::lock_guard<std::mutex> lock(queueMutex);
        return static_cast<int>(pendingTiles.size() + readyQueue.size() + finalizingTiles_.size());
    }
    [[nodiscard]] TileCoord getCurrentTile() const { return currentTile; }

    /** Process one ready tile (for loading screens with per-tile progress updates) */
    void processOneReadyTile();

    /** Process a bounded batch of ready tiles with async GPU upload (no sync wait) */
    void processReadyTiles();

    /** Unload a bounded batch of distant tiles queued by streamTiles() (no frame-spike) */
    void processPendingUnloads();

private:
    /**
     * Get tile coordinates from GL world position
     */
    [[nodiscard]] TileCoord worldToTile(float worldX, float worldY) const;

    /**
     * Get world bounds for a tile
     */
    void getTileBounds(const TileCoord& coord, float& minX, float& minY,
                       float& maxX, float& maxY) const;

    /**
     * Build ADT file path
     */
    [[nodiscard]] std::string getADTPath(const TileCoord& coord) const;

    /**
     * Load tiles in radius around current tile
     */
    void streamTiles();

    /**
     * Background thread: prepare tile data (CPU work only, no OpenGL)
     */
    std::shared_ptr<PendingTile> prepareTile(int x, int y, TileDetail detail);

    /**
     * Advance incremental finalization of a tile (one bounded unit of work).
     * Returns true when the tile is fully finalized (phase == DONE).
     */
    bool advanceFinalization(FinalizingTile& ft);

    /**
     * Background worker thread loop
     */
    void workerLoop();

    void loadGroundEffectTables();

    pipeline::AssetManager* assetManager = nullptr;
    const pipeline::GameTables* gameTables_ = nullptr;
    TerrainRenderer* terrainRenderer = nullptr;
    LiquidScene* liquidScene = nullptr;
    M2Renderer* m2Renderer = nullptr;
    WMORenderer* wmoRenderer = nullptr;
    collision::CollisionWorld* collision_ = nullptr;
    /// How the workers encode WMO normal maps: the device's, read once when
    /// the renderer is set rather than from the workers.
    NormalMapFormat wmoNormalMapFormat_ = NormalMapFormat::Rgba8;
    /// The same for the ground's relief, read when the terrain renderer is set.
    NormalMapFormat terrainNormalMapFormat_ = NormalMapFormat::Rgba8;
    AmbientEmitterSink ambientEmitterSink_;

    std::string mapName = "Azeroth";

    // Loaded tiles (keyed by coordinate)
    std::unordered_map<TileCoord, std::unique_ptr<TerrainTile>, TileCoord::Hash> loadedTiles;

    // Tiles that failed to load (don't retry)
    std::unordered_map<TileCoord, bool, TileCoord::Hash> failedTiles;

    // Current tile (where camera is)
    TileCoord currentTile = {.x = -1, .y = -1};
    TileCoord lastStreamTile = {.x = -1, .y = -1};

    // Streaming parameters
    bool streamingEnabled = true;
    bool streamingPolicyDirty_ = true;
    int landmarkRadius_ = 0;
    int loadRadius = 6;      // Load tiles within this radius (13x13 grid = 169 tiles)
    /// Time one finalization step may take before it yields, set per frame by
    /// processReadyTiles from how near the tile being finished is.
    float stepBudgetMs_ = 4.0f;
    /// What unloading tiles cost this frame by part (M2 instances, WMO
    /// instances, terrain chunks, water, the tile's parsed data);
    /// processPendingUnloads hands them to the frame profile and starts them
    /// again.
    std::array<float, 5> unloadPartMs_{};
    /// The same for finishing tiles, by what inside a phase the time went
    /// to; processReadyTiles hands them on and starts them again.
    enum FinalizePart : size_t {
        kPartTerrainTextures, kPartTerrainChunks, kPartTerrainWater,
        kPartM2ModelLoad, kPartM2InstanceCreate, kFinalizePartCount
    };
    std::array<float, kFinalizePartCount> finalizePartMs_{};
    /// Tiles around the neighbour the camera is about to cross into, queued
    /// before it gets there. Cleared on each crossing.
    std::vector<TileCoord> prefetchedCenters_;
    /// Queues every tile within loadRadius of center that is not loaded,
    /// pending or known missing, nearest first.
    /// urgent puts them ahead of what is already queued; a prefetch goes
    /// behind it.
    void enqueueAround(const TileCoord& center, bool urgent);
    /// Queues the tiles around the neighbours the camera is close to.
    void prefetchAhead(const glm::vec3& camPos);
    int unloadRadius = 9;    // Unload tiles beyond this radius
    float updateInterval = 0.033f;  // Check streaming every 33ms (~30 fps)
    float timeSinceLastUpdate = 0.0f;
    float proactiveStreamTimer_ = 0.0f;
    bool taxiStreamingMode_ = false;
    bool isCustomZone_ = false;

    // Collision data for custom zones (loaded from WOC files)
    struct CollisionData {
        struct Triangle { glm::vec3 v0, v1, v2; uint8_t flags; };
        std::vector<Triangle> triangles;
        glm::vec3 boundsMin{1e30f}, boundsMax{-1e30f};
        bool loaded = false;
    };
    std::unordered_map<uint64_t, CollisionData> collisionTiles_;
    [[nodiscard]] uint64_t tileKey(int x, int y) const { return (static_cast<uint64_t>(x) << 32) | static_cast<uint32_t>(y); }

    // Tile size constants (WoW ADT specifications)
    // A tile (ADT) = 16x16 chunks = 533.33 units across
    // A chunk = 8x8 vertex quads = 33.33 units across
    static constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    static constexpr float CHUNK_SIZE = 33.33333f;          // One chunk = 33.33 units

    // Background loading worker pool
    std::vector<std::thread> workerThreads;
    int workerCount = 0;
    // THREAD-SAFE: guards loadQueue, readyQueue, and pendingTiles.
    // Workers wait on queueCV; main thread signals when new tiles are enqueued
    // or when readyQueue drains below maxReadyQueueSize_.
    mutable std::mutex queueMutex;
    std::condition_variable queueCV;
    std::deque<TileCoord> loadQueue;              // THREAD-SAFE: protected by queueMutex
    std::queue<std::shared_ptr<PendingTile>> readyQueue; // THREAD-SAFE: protected by queueMutex
    // Maximum number of prepared-but-not-finalized tiles in readyQueue.
    // Each prepared tile can hold 100–500 MB of decoded textures in RAM.
    // Workers sleep when this limit is reached, letting the main thread
    // finalize (GPU-upload + free) before more tiles are prepared.
    static constexpr size_t maxReadyQueueSize_ = 3;

    // A revisited tile is prepared again. Its files - the ADT, its models and
    // buildings - come from the asset manager's file cache when they are
    // still there; a whole prepared tile is not kept, since finalizing hands
    // its contents to the GPU and the decoded textures it holds would be a
    // second copy of hundreds of megabytes.
    void logMissingAdtOnce(const std::string& adtPath);
    std::atomic<bool> workerRunning{false};
    core::JobGroup standaloneJobs_;
    core::JobGroup::Token mapToken_ = standaloneJobs_.token();

    // Track tiles currently queued or being processed to avoid duplicates
    std::unordered_map<TileCoord, TileDetail, TileCoord::Hash> pendingTiles; // THREAD-SAFE: protected by queueMutex
    // Tiles a worker has taken and not yet handed to readyQueue. Protected by queueMutex.
    int preparingTiles_ = 0;
    // Whether the workers are waiting on memory, so the wait is reported once
    // per episode rather than once per retry. Protected by queueMutex.
    bool memoryWaitReported_ = false;
    std::unordered_set<std::string> missingAdtWarnings_; // THREAD-SAFE: protected by missingAdtWarningsMutex_
    std::mutex missingAdtWarningsMutex_;

    // Thread-safe set of M2 model IDs already uploaded to GPU
    // (checked by workers to skip redundant file I/O + parsing)
    std::unordered_set<uint32_t> uploadedM2Ids_;
    std::mutex uploadedM2IdsMutex_;


    // Cross-tile dedup for WMO doodad preparation on background workers
    // (prevents re-parsing thousands of doodads when same WMO spans multiple tiles)
    std::unordered_set<uint32_t> preparedWmoUniqueIds_;
    std::mutex preparedWmoUniqueIdsMutex_;

    // MAIN-THREAD-ONLY: tiles beyond unloadRadius, queued by streamTiles() and drained a
    // time-budgeted batch at a time by processPendingUnloads() each frame. Unloading them
    // all synchronously in one call (e.g. ~100 tiles right after a taxi landing snaps the
    // radius down) caused multi-second main-thread stalls - live-confirmed via "SLOW
    // terrainManager->update: 1943.71ms" immediately after "Unloaded 103 distant tiles" in
    // a real flight-landing log.
    //
    // This was originally a fixed per-frame *count* cap (8 tiles/frame) rather than a time
    // budget. That prevented the single-frame catastrophic stall but doesn't scale with
    // actual frame cost: streamTiles() discovers newly-out-of-range tiles at a roughly
    // constant real-world rate during a long, fast (taxi) flight, but a count-based drain
    // processes fewer tiles per *second* whenever frame rate drops for any reason - and once
    // the backlog (and therefore loadedTiles_/streamTiles()'s own per-call scan cost) starts
    // growing, frame rate drops further, which throttles the count-based drain further still.
    // Live-reproduced: a long cross-continent taxi flight (the first sustained-large-radius
    // tile churn any test had done) snowballed from ~50ms terrain-update frames early on to
    // 600ms+ later in the same flight. A time budget (matching processReadyTiles()/
    // advanceFinalization()'s existing pattern) scales unload throughput with whatever time
    // is actually available each frame instead of a fixed count, so it can't fall behind
    // real-world churn the way the count cap could.
    std::deque<TileCoord> pendingUnloadQueue_;

    // MAIN-THREAD-ONLY: checked and modified in processReadyTiles() and unloadDistantTiles(),
    // both of which run exclusively on the main thread.
    TerrainPlacementRefs placedDoodadIds;

    // MAIN-THREAD-ONLY: same contract as placedDoodadIds.
    TerrainPlacementRefs placedWmoIds;

    // Tiles currently being incrementally finalized across frames
    std::deque<FinalizingTile> finalizingTiles_;

    // Filled by initialize() before the workers start, read-only after: every
    // worker reads them while it prepares a tile.
    pipeline::GroundEffectTables groundEffects_;

    DetailDoodadRenderer* detailDoodadRenderer_ = nullptr;
    /// The detail doodads whose models have gone to the renderer, so a worker
    /// preparing a tile does not read them again. Workers read it, the main
    /// thread adds to it as tiles are committed.
    mutable std::mutex detailModelsMutex_;
    std::unordered_set<uint32_t> detailModelsDelivered_;
    /// Read the models of the detail doodads the tile's ground effects plant
    /// and have not been delivered: on the worker preparing the tile.
    void prepareDetailDoodadModels(PendingTile& pending);
};

} // namespace rendering
} // namespace wowee
