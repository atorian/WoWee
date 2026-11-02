#pragma once

#include "rendering/view_distance.hpp"
#include "rendering/terrain_depth.hpp"

#include <cmath>

#include "rendering/texture_cache.hpp"
#include "rendering/normal_map_encoding.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/local_lights.hpp"
#include "rendering/range_allocator.hpp"
#include "rendering/terrain_bake.hpp"
#include "rendering/terrain_tile_maps.hpp"
#include "pipeline/distant_terrain.hpp"
#include "rendering/terrain_chunk_index.hpp"
#include "rendering/frames_in_flight.hpp"
#include "rendering/geometry_pool.hpp"
#include "rendering/gpu_slot_table.hpp"
#include "rendering/vk_utils.hpp"

#include "pipeline/terrain_mesh.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/camera.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>

namespace wowee {

// Forward declarations
namespace pipeline { class AssetManager; }

namespace rendering {

class VkContext;
class VkTexture;
class Frustum;
class RtScene;
struct WaterReflectionView;


/// A tile's chunk maps on the GPU: what every chunk's three blended layers
/// cover and every chunk's normals, one array layer to a chunk
/// (TerrainChunkSlot). Two images for the tile instead of up to four small
/// ones for each of its 256 chunks, which a driver keeps an object and a
/// descriptor's worth of bookkeeping for apiece.
struct TerrainTileMaps {
    /// RGBA8 64x64 by TerrainChunkSlot::kCount: TerrainTileMapPixels::cover.
    AllocatedImage cover;
    /// RGBA8 17x17 by TerrainChunkSlot::kCount: TerrainTileMapPixels::normals.
    AllocatedImage normals;
    /// Their places in the texture table, given back with the images.
    TextureHandle coverHandle = kNoTextureHandle;
    TextureHandle normalsHandle = kNoTextureHandle;
};

/// What the terrain shaders read of a chunk (TerrainChunk in
/// terrain_layers.glsli, std430): its textures by handle - the base and the
/// three layers over it, then their relief - its tile's maps and bake atlas,
/// and how many layers lie over the base and its layer in the tile's maps.
struct TerrainChunkRowGPU {
    uint32_t color[4] = {kNoTextureHandle, kNoTextureHandle, kNoTextureHandle, kNoTextureHandle};
    uint32_t relief[4] = {kNoTextureHandle, kNoTextureHandle, kNoTextureHandle, kNoTextureHandle};
    /// Cover, normals, baked colour, baked relief.
    uint32_t maps[4] = {kNoTextureHandle, kNoTextureHandle, kNoTextureHandle, kNoTextureHandle};
    int32_t layerCount = 0;
    int32_t mapLayer = 0;
    int32_t pad[2] = {};
};
static_assert(sizeof(TerrainChunkRowGPU) == 64, "TerrainChunk in terrain_layers.glsli");

/// What the ground's cull reads of a chunk (CullRow in terrain_cull.comp.glsl,
/// std430), at the same row as its TerrainChunkRowGPU.
struct TerrainCullRowGPU {
    glm::vec4 sphere{0.0f};          ///< centre, radius
    glm::vec4 lodErrors{0.0f};       ///< by level, in yards
    uint32_t neighbours[4] = {~0u, ~0u, ~0u, ~0u};  ///< rows across edges y-1, y+1, x-1, x+1
    uint32_t group = 0;              ///< its geometry page
    uint32_t firstIndex = 0;
    int32_t baseVertex = 0;
    uint32_t offeredLods = 1;
    uint32_t cell = 0;               ///< chunkX + 16 chunkY, its cell in the tile's atlas
    uint32_t flags = 0;              ///< kValid, kBaked
    float steepestUpZ = 1.0f;
    uint32_t pad = 0;
    uint32_t lods[4][2] = {};        ///< first, count of each level's surface
    uint32_t skirts[16][2] = {};     ///< [level * 4 + edge]

    static constexpr uint32_t kValid = 1u;
    static constexpr uint32_t kBaked = 2u;
};
static_assert(sizeof(TerrainCullRowGPU) == 240, "CullRow in terrain_cull.comp.glsl");

/// What the ground's shaders read of one chunk's draws (terrain_cull.comp.glsl
/// writes it, the draw's first instance names it).
struct TerrainDrawRecordGPU {
    uint32_t row[4] = {};
    glm::vec4 relief{0.0f};
    glm::vec4 baked{0.0f};
    glm::uvec4 lights{0u};
};
static_assert(sizeof(TerrainDrawRecordGPU) == 64, "DrawRecord in terrain_cull.comp.glsl");

/// The cull's view of the frame (Frame in terrain_cull.comp.glsl, std140).
struct TerrainCullFrameGPU {
    glm::vec4 viewPlanes[6]{};
    glm::vec4 mirrorPlanes[6]{};
    glm::vec4 camera{0.0f};   ///< position, focal length in pixels
    glm::vec4 limits{0.0f};   ///< reach, where the discards start, the bake band
    glm::vec4 mirror{0.0f};   ///< reflection drawn, water height, relief strength, frustum-culled
    glm::uvec4 counts{0u};    ///< rows, lights, levels of detail, bake
    glm::uvec4 groups{0u};    ///< groups
};

/// What the cull counted, for the frame's statistics (Stats in
/// terrain_cull.comp.glsl).
struct TerrainCullStatsGPU {
    uint32_t drawn = 0, culled = 0, baked = 0, crossfade = 0;
    uint32_t furthestSq = 0;  ///< float bits
    uint32_t triangles = 0, draws = 0, records = 0;
};

/// Where a chunk's maps are: its tile's arrays, and its layer in them.
struct TerrainChunkMaps {
    std::shared_ptr<const TerrainTileMaps> tile;
    TerrainChunkSlot slot;
};

struct TerrainChunkGPU {
    /// Level 0, the full mesh, at the start of the index buffer: what the
    /// shadow pass and anything else that wants the true ground draws.
    uint32_t indexCount = 0;
    /// Each level of detail's surface in the index buffer, level 0 first; a
    /// level with no indices is not offered.
    struct LodRange {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::array<LodRange, pipeline::kTerrainLodLevels> lods{};
    /// Each level's skirt along each edge (row 0, row 8, column 0, column 8),
    /// drawn where the neighbour across that edge is at another level.
    std::array<std::array<LodRange, 4>, pipeline::kTerrainLodLevels> skirts{};
    /// How far each level strays from the ground, in yards (ChunkMesh::lodError).
    std::array<float, pipeline::kTerrainLodLevels> lodErrors{};
    uint32_t offeredLods = 1u;
    /// What its layers cover and its normals, which the ground is lit from at
    /// every level: a layer of its tile's maps.
    TerrainChunkMaps maps;

    static constexpr uint32_t kNoRow = ~0u;
    /// Its row in the renderer's chunk table (TerrainChunkRowGPU): what the
    /// shaders read of it, its textures by handle.
    uint32_t row = kNoRow;
    /// What its row holds for the shaders, kept to be sent again with the
    /// cull's half when a neighbour or its bake changes.
    TerrainChunkRowGPU shading;

    // The chunk's textures, held: what its row names stays resident as long
    // as the chunk does.
    TextureRef baseTexture;
    TextureRef layerTextures[3];
    /// The relief of the base and layer textures, derived from their colours;
    /// empty where none could be had, which binds a flat one.
    TextureRef baseNormalMap;
    TextureRef layerNormalMaps[3];
    int layerCount = 0;

    // World position for culling
    float worldX = 0.0f;
    float worldY = 0.0f;
    float worldZ = 0.0f;

    // Owning tile coordinates (for per-tile removal)
    int tileX = -1, tileY = -1;
    /// The chunk's place in its tile (ChunkMesh::chunkX/chunkY): with the
    /// tile, where its neighbours are.
    int chunkX = 0, chunkY = 0;

    // Bounding sphere for frustum culling
    float boundingSphereRadius = 0.0f;
    glm::vec3 boundingSphereCenter = glm::vec3(0.0f);

    /// Where its geometry lives in the renderer's GeometryPool: every index
    /// it holds, all levels and their skirts. Unplaced when there was no room
    /// anywhere, and then the chunk is not drawn.
    GeometryPool::Range geometry;

    // The chunk as the ray traced lighting sees it (RtScene ids, or ~0u).
    uint32_t rtMesh = ~0u;
    uint32_t rtInstance = ~0u;

    /// Its cell of its tile's bake atlas holds it (TerrainBake): from now on
    /// it may be drawn far from the bake. Until then, from its layers.
    std::shared_ptr<std::atomic<bool>> bakeComplete = std::make_shared<std::atomic<bool>>(false);
    /// The up-component of its steepest normal, 1 flat: how much nearer than
    /// flat ground its steepest slope reads its layers finely
    /// (terrainSurfaceSourceFor).
    float steepestUpZ = 1.0f;

    [[nodiscard]] bool isValid() const { return geometry.placed(); }
};

/**
 * Terrain renderer (Vulkan)
 */
class TerrainRenderer {
public:
    TerrainRenderer();
    ~TerrainRenderer();

    /**
     * Initialize terrain renderer
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for set 0 (per-frame UBO)
     * @param assetManager Asset manager for loading textures
     */
    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assetManager);

    void shutdown();

    bool loadTerrain(const pipeline::TerrainMesh& mesh,
                     const std::vector<std::string>& texturePaths,
                     int tileX = -1, int tileY = -1);


    /// Upload a batch of terrain chunks incrementally. Returns true when all chunks done.
    /// chunkIndex is updated to the next chunk to process (0-255 row-major).
    bool loadTerrainIncremental(const pipeline::TerrainMesh& mesh,
                                const std::vector<std::string>& texturePaths,
                                int tileX, int tileY,
                                int& chunkIndex, int maxChunksPerCall = 16);

    void removeTile(int tileX, int tileY);

    void uploadPreloadedTextures(const std::unordered_map<std::string, pipeline::BLPImage>& textures);

    /// The relief of a tileset texture, derived from its colours on a loading
    /// worker and encoded with its mips. Pure: no Vulkan, safe off the main
    /// thread. Invalid when the texture has no pixels to read.
    ///
    /// The ground's relief used to be made in the fragment shader from the
    /// screen-space derivatives of the shaded colour. A 2x2 pixel quad then
    /// decided where a cobble's edge was, so the relief changed with every
    /// sub-pixel move of the camera: blocky noise that crawled over roads and
    /// grass as the player rode. Derived here it belongs to the texture, is
    /// filtered by its mips like the colour is, and holds still.
    [[nodiscard]] static EncodedNormalMap deriveNormalMap(const pipeline::BLPImage& blp, NormalMapFormat format);
    /// The cache key a tileset texture's relief is kept under.
    [[nodiscard]] static TextureKey normalMapKey(const std::string& path);
    /// Puts a normal map made by deriveNormalMap in the shared cache, where
    /// the chunks that lay its texture find it.
    void uploadPreloadedNormalMap(const std::string& path, const EncodedNormalMap& map);
    [[nodiscard]] NormalMapFormat normalMapFormat() const;

    /// The "Surface bumps" setting: the ground lit by its textures' relief, as
    /// strongly as `strength` asks (1 as derived, 2 twice as steep). Off is
    /// flat ground lit by its vertex normals alone, as the original client
    /// draws it.
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; }
    void setNormalMapStrength(float strength) { normalMapStrength_ = std::clamp(strength, 0.0f, 2.0f); }

    /**
     * Render terrain
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0)
     * @param camera Camera for frustum culling
     */
    struct GeometryDraw {
        VkBuffer vertices = VK_NULL_HANDLE, indices = VK_NULL_HANDLE;
        uint32_t firstIndex = 0, count = 0;
        int32_t baseVertex = 0;
    };
    /// One geometry page's draws: its buffers, and how many commands each
    /// of its four lists - the view without the distance discards and with
    /// them, the reflection likewise - has room for (five a chunk).
    struct GroupDraw {
        ::VkBuffer vertices = VK_NULL_HANDLE, indices = VK_NULL_HANDLE;
        uint32_t capacity = 0;
    };
    using DepthDraw = TerrainDepthChunk<GeometryDraw>;
    using DepthTile = TerrainDepthTile<GeometryDraw>;
    struct BakeDraw {
        GeometryDraw geometry;
        uint32_t row = TerrainChunkGPU::kNoRow;
        int tileX = 0, tileY = 0, chunkX = 0, chunkY = 0;
        TerrainBake::Atlas* atlas = nullptr;
        std::shared_ptr<std::atomic<bool>> complete;
    };
    struct DrawList {
        /// What the cull on the GPU needs of the frame (dispatchCull), and
        /// where each page's lists of commands start in the frame's command
        /// buffer: four a page, view then reflection, without the discards
        /// then with them.
        TerrainCullFrameGPU cull;
        std::vector<GroupDraw> groups;
        std::vector<uint32_t> regions;
        uint32_t commandCapacity = 0;
        std::vector<std::shared_ptr<const DepthTile>> depth;
        std::vector<BakeDraw> bake;
        std::vector<std::shared_ptr<TerrainBake::Atlas>> atlases;
        /// The chunk table's rows not yet known to be on the GPU, to be
        /// copied in before this frame draws (uploadChunkRows), and how many
        /// rows the table holds.
        std::vector<uint32_t> rowSlots;
        std::vector<TerrainChunkRowGPU> rows;
        std::vector<TerrainCullRowGPU> cullRows;
        uint32_t rowCount = 0;
        uint64_t rowsExtraction = 0;
        bool wireframe = false;
    };
    DrawList extract(const Camera& camera, const std::vector<LocalLight>& lights,
                     const WaterReflectionView* mirror = nullptr);
    void recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list);
    /// The chunks the water's reflection can show, through its mirrored set.
    void recordReflection(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list);
    void recordDepth(VkCommandBuffer cmd, const glm::mat4& matrix, const glm::vec3& centre,
                     float radius, const DrawList& list);

    /// Outside any rendering scope, before the frame's terrain is drawn: the
    /// atlases made since the last frame readied, and the chunks waiting for
    /// their bake drawn into them, a budget a frame (TerrainBake).
    void recordBake(VkCommandBuffer cmd, const DrawList& list);
    /// Outside any rendering scope, before anything of the frame's terrain
    /// is recorded: the chunk rows the list carries copied into the table.
    void uploadChunkRows(VkCommandBuffer cmd, uint32_t frameIndex, const DrawList& list);
    /// Outside any rendering scope, after uploadChunkRows and before the
    /// view and the reflection are recorded: the cull that makes their
    /// draws, reading `lights` - the frame's point lights, `lightCount` of
    /// them - to pick each chunk's. Reads back what the cull of this frame
    /// slot's last frame counted.
    void dispatchCull(VkCommandBuffer cmd, uint32_t frameIndex, const DrawList& list, ::VkBuffer lights,
                      uint32_t lightCount);
    /// The frame that carried the last upload was submitted: its rows count
    /// as on the GPU, and later lists leave them out.
    void frameSubmitted();
    /// Chunks the last render() drew from their bake alone, and across the
    /// band from both it and their layers.
    [[nodiscard]] int getBakedDrawnChunkCount() const { return bakedDrawnChunks_; }
    [[nodiscard]] int getCrossfadeDrawnChunkCount() const { return crossfadeDrawnChunks_; }
    /// Video memory the tiles' bake atlases hold between them.
    [[nodiscard]] uint64_t getBakeAtlasBytes() const { return bake_.atlasBytes(); }

    /**
     * Initialize terrain shadow pipeline (must be called after initialize()).
     * @param shadowRenderPass  Depth-only render pass used for the shadow map.
     */
    bool initializeShadow(VkRenderPass shadowRenderPass);

    [[nodiscard]] bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    [[nodiscard]] GeometryPool::Stats geometryStats() const { return geometry_.stats(); }

    void clear();

    void recreatePipelines();
    /// The fill pipeline and its wireframe derivative, which
    /// initialize() and recreatePipelines() both need.
    bool buildMainPassPipelines(VkDevice device,
                                wowee::rendering::VkShaderModule& vertShader,
                                wowee::rendering::VkShaderModule& fragShader);

    void setWireframe(bool enabled) { wireframe = enabled; }
    /// The frame's point lights, which every chunk picks its nearest from.

    void setFrustumCulling(bool enabled) { frustumCullingEnabled = enabled; }
    void setFogEnabled(bool enabled) { fogEnabled = enabled; }
    [[nodiscard]] bool isFogEnabled() const { return fogEnabled; }
    void setViewDistance(float distance) { maxViewDistance_ = std::clamp(distance, kMinViewDistance, kMaxViewDistance); }
    [[nodiscard]] float viewDistance() const { return maxViewDistance_; }

    void setShadowMap(VkDescriptorImageInfo /*depthInfo*/, const glm::mat4& /*lightSpaceMat*/) {}
    void clearShadowMap() {}

    [[nodiscard]] int getChunkCount() const { return static_cast<int>(chunks.size()); }
    [[nodiscard]] int getRenderedChunkCount() const { return renderedChunks; }
    /// How far the furthest chunk drawn this frame was, for the diagnostic that
    /// compares it against the doodads. See Renderer::logViewDistanceDiag.
    [[nodiscard]] float getFurthestDrawnDistance() const { return std::sqrt(furthestDrawnSq_); }
    [[nodiscard]] int getCulledChunkCount() const { return culledChunks; }
    /// The triangles the last render() drew, at the levels it chose.
    [[nodiscard]] uint64_t getRenderedTriangleCount() const { return renderedTriangles_; }
    /// The draw calls the last render() recorded: a chunk's level and each
    /// skirt it draws are one apiece.
    [[nodiscard]] uint32_t getDrawCallCount() const { return renderedDraws_; }
    [[nodiscard]] int getTriangleCount() const;

    /// The height in pixels of the image the terrain is drawn into, which the
    /// level of detail is chosen against: a chunk is drawn at the coarsest
    /// level whose error stays within a pixel on screen (terrain_cull.comp.glsl).
    void setLodViewportHeight(uint32_t pixels) { lodViewportHeight_ = pixels; }
    /// Each tile's resident chunks, which the distant terrain leaves alone.
    [[nodiscard]] const pipeline::TerrainCoverage& detailedCoverage() const { return chunkIndex_.coverage(); }
    [[nodiscard]] VkContext* getVkContext() const { return vkCtx; }

    /// Where chunks register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }

private:

    /// Records the cull's draws of the view (reflection false) or the
    /// reflection: per page, a call per pipeline.
    void recordCulled(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list, bool reflection);
    /// The cull's row of the chunk at `index` in `chunks`, from it and its
    /// neighbours, queued with its shading row for the GPU.
    void queueRows(uint32_t index);
    bool createCull();
    void destroyCull();
    GeometryDraw geometryOf(const TerrainChunkGPU& chunk) const;
    static void bindDraw(VkCommandBuffer cmd, const GeometryDraw& draw);
    TerrainChunkGPU uploadChunk(const pipeline::ChunkMesh& chunk);
    /// The tileset texture from the shared cache, loading it if it is not
    /// there; white when it cannot be had.
    TextureRef loadTexture(const std::string& path);
    /// The tileset texture's relief from the shared cache. Normally the
    /// loading worker made it; when the cache let it go since, it is derived
    /// again here, on this thread. Empty when it cannot be had.
    TextureRef loadNormalMap(const std::string& path);
    /// The tile's chunk maps, made and uploaded from the whole mesh the first
    /// time one of its chunks is, before any of them is drawn. Inside an
    /// upload batch. The fallback maps when the tile's cannot be made, which
    /// the log says once.
    std::shared_ptr<const TerrainTileMaps> tileMapsFor(int tileX, int tileY,
                                                       const pipeline::TerrainMesh& mesh);
    /// Lets the tile's maps go once the frames that may draw them are done.
    void releaseTileMaps(int tileX, int tileY);
    void calculateBoundingSphere(TerrainChunkGPU& chunk, const pipeline::ChunkMesh& meshChunk);
    /// Gives the chunk a row and queues what it holds for the GPU. False
    /// when there is nothing sampleable to name, in which case the caller
    /// must drop the chunk: a row naming no texture is as undefined to draw
    /// as a null image view.
    bool writeChunkRow(TerrainChunkGPU& chunk);
    void destroyChunkGPU(TerrainChunkGPU& chunk);

    /// Point a chunk's base, layer and alpha textures at the loaded ones.
    ///
    /// Both load paths do this identically; chunkX and chunkY only name the
    /// chunk in the warnings. A layer whose textureId is past the end of the
    /// tile's texture list falls back to white rather than going unbound.
    void bindChunkTextures(TerrainChunkGPU& gpuChunk,
                           const pipeline::ChunkMesh& chunk,
                           const std::vector<std::string>& texturePaths,
                           int tileX, int tileY, int chunkX, int chunkY);

    VkContext* vkCtx = nullptr;
    RtScene* rtScene_ = nullptr;
    void registerRtChunk(TerrainChunkGPU& gpuChunk, const pipeline::ChunkMesh& chunk);
    pipeline::AssetManager* assetManager = nullptr;

    // Main pipelines
    VkPipeline pipeline = VK_NULL_HANDLE;
    /// The fill pipeline without the distance discards, for chunks that end
    /// short of them. Null if it failed to build; those chunks then take the
    /// fill pipeline.
    VkPipeline shortOfDiscardsPipeline = VK_NULL_HANDLE;
    VkPipeline wireframePipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    // Shadow pipeline
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    /// Every resident chunk's row (TerrainChunkRowGPU), read by the ground's
    /// and the bake's fragment shaders through a set per frame slot that is
    /// rewritten when the table is replaced. Rows are handed out on the main
    /// thread and given back with the chunk; a row given to another chunk is
    /// copied in by a later frame, after every earlier frame's draws.
    GpuSlotTable chunkRows_{sizeof(TerrainChunkRowGPU), "terrain chunk table"};
    VkDescriptorSetLayout chunkSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool chunkSetPool_ = VK_NULL_HANDLE;
    VkDescriptorSet chunkSets_[MAX_FRAMES_IN_FLIGHT] = {};
    std::vector<uint32_t> freeRows_;
    uint32_t rowCount_ = 0;
    /// Rows changed and not yet known to be on the GPU, with the first
    /// extraction that carries them. Main thread.
    struct PendingRow {
        TerrainChunkRowGPU row;
        TerrainCullRowGPU cull;
        uint64_t extraction = 0;
    };
    std::unordered_map<uint32_t, PendingRow> pendingRows_;
    uint64_t extractions_ = 0;
    /// The extraction whose rows the last upload copied, until its frame is
    /// submitted (render thread); then the last one submitted, read by
    /// extract on the main thread.
    uint64_t rowsUploaded_ = 0;
    std::atomic<uint64_t> rowsSubmitted_{0};

    /// The ground's cull (terrain_cull.comp.glsl): its rows beside the chunk
    /// rows, and per frame slot what it reads and writes - the per-row levels,
    /// the draw records, the commands, each list's start and count, what it
    /// counted (read back two frames later) and the frame's view.
    GpuSlotTable cullRows_{sizeof(TerrainCullRowGPU), "terrain cull table"};
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout cullPipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool cullSetPool_ = VK_NULL_HANDLE;
    struct CullFrame {
        VkDescriptorSet set = VK_NULL_HANDLE;
        AllocatedBuffer levels, records, commands, regions, counts, stats, view;
        uint32_t rowCapacity = 0, commandCapacity = 0, groupCapacity = 0;
        /// Versions of the tables its sets name as they were written.
        uint32_t cullRowsVersion = 0, chunkRowsVersion = 0;
        bool setsCurrent = false;
        /// It ran this frame: the view and the reflection may draw from it.
        bool culled = false;
        /// Its stats hold a frame's counts not yet read.
        bool counted = false;
    };
    CullFrame cullFrames_[MAX_FRAMES_IN_FLIGHT];
    /// Chunks resident in each geometry page, the room its lists need.
    std::vector<uint32_t> pageChunks_;
    /// Draws' counts come from the GPU (drawIndirectCount); without it each
    /// list is drawn at its full room, the rest of it empty commands.
    bool countFromGpu_ = false;

    // Loaded terrain chunks
    std::vector<TerrainChunkGPU> chunks;

    /// Tileset paths that would not load, said once each.
    std::unordered_set<std::string> loggedTextureLoadFails_;

    // Fallback textures, held in the shared cache like any other.
    TextureRef whiteTexture;
    TextureRef flatNormalTexture;
    /// Each resident tile's chunk maps, by tile (chunkMapKey).
    std::unordered_map<uint64_t, std::shared_ptr<TerrainTileMaps>> tileMaps_;
    /// One layer that covers everything and is lit flat, for a tile whose
    /// maps could not be made: its layers drawn over its base, as a chunk
    /// whose alpha map failed was.
    std::shared_ptr<TerrainTileMaps> fallbackTileMaps_;
    /// Linear, clamped to the edge, for both arrays.
    VkSampler tileMapSampler_ = VK_NULL_HANDLE;
    bool tileMapsFailureSaid_ = false;
    /// Tileset paths whose relief could not be derived, said once each.
    std::unordered_set<std::string> loggedNormalMapFails_;

    // Rendering state
    bool wireframe = false;
    bool normalMappingEnabled_ = true;
    float normalMapStrength_ = 0.8f;
    bool frustumCullingEnabled = true;
    bool fogEnabled = true;
    float maxViewDistance_ = 1200.0f;
    /// WOWEE_TERRAIN_LOD=0 draws every chunk at level 0, to compare against.
    bool lodEnabled_ = true;
    /// The far ground's atlases. WOWEE_TERRAIN_BAKE=0 draws every chunk from
    /// its layers, to compare against.
    TerrainBake bake_;
    bool bakeEnabled_ = false;
    /// Chunks waiting for their bake, by their place on the map (gx, gy).
    std::vector<std::pair<int, int>> bakeQueue_;
    int bakedDrawnChunks_ = 0;
    int crossfadeDrawnChunks_ = 0;
    /// Enqueue a chunk just added for its bake.
    void queueBake(const TerrainChunkGPU& chunk);
    /// Puts a chunk just made at the end of `chunks`, records its place in
    /// chunkIndex_ and its tile's list, and queues its bake.
    void addChunk(TerrainChunkGPU&& chunk);
    /// Where each chunk is in `chunks`, by its place on the map, its
    /// neighbours there and the coverage, kept up as chunks come and go:
    /// rebuilt from all of them whenever the list changed, it was
    /// 30,000-40,000 inserts on every frame a tile streamed in or out.
    TerrainChunkIndex chunkIndex_;
    /// The chunks of each tile and a sphere round all of theirs: the shadow
    /// pass tests a tile before its chunks, and a tile outside the map has no
    /// chunk inside it. Kept up with chunkIndex_.
    struct ShadowTile {
        int tileX = 0, tileY = 0;
        std::vector<uint32_t> chunks;
        TerrainDepthCache<GeometryDraw> depth;
    };
    std::vector<ShadowTile> shadowTiles_;
    /// Which of shadowTiles_ each tile is, by tile.
    std::unordered_map<uint64_t, uint32_t> tileAt_;
    /// This frame's level for each chunk in `chunks`, -1 where it is not drawn.

    /// The visible chunks of this frame, in `chunks` order, with their levels.

    uint32_t lodViewportHeight_ = 0;
    uint64_t renderedTriangles_ = 0;
    uint32_t renderedDraws_ = 0;
    int renderedChunks = 0;
    float furthestDrawnSq_ = 0.0f;
    int culledChunks = 0;

    // The chunks' geometry, in pages each holding many tiles: a call draws a
    // page's chunks. A tile is 256 chunks, each with every level of detail
    // and its skirts (pipeline::kChunkMaxVertices, kChunkMaxIndices).
    static constexpr uint32_t kTilesPerGeometryPage = 16;
    static constexpr uint32_t kPageVertices = kTilesPerGeometryPage * 256 * pipeline::kChunkMaxVertices;
    static constexpr uint32_t kPageIndices = kTilesPerGeometryPage * 256 * pipeline::kChunkMaxIndices;
    GeometryPool geometry_{{.what = "Terrain", .vertexStride = sizeof(pipeline::TerrainVertex),
                            .indexType = VK_INDEX_TYPE_UINT32, .pageVertices = kPageVertices,
                            .pageIndices = kPageIndices}};
    /// Binds the page's buffers.

};

} // namespace rendering
} // namespace wowee
