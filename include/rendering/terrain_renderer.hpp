#include "rendering/surface_identity.hpp"
#pragma once
#include "pipeline/prepared_texture.hpp"

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
#include "pipeline/distant_terrain.hpp"
#include "rendering/uniform_slot_pool.hpp"
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
class SurfacePass;

/**
 * GPU-side terrain chunk data (Vulkan)
 */
struct TerrainSurfacePush {
    /// x: how steep the textures' relief is drawn, 0 for flat ground.
    glm::vec4 relief{0.0f};
    /// x: where the colour and relief come from (TerrainSurfaceSource);
    /// y, z: the band the bake takes over across, in yards from the camera;
    /// w: the chunk's cell in its tile's atlas, x + 16 y.
    glm::vec4 baked{0.0f};
};

struct TerrainChunkGPU {
    uint64_t surfaceIdentity = nextSurfaceIdentity();
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
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
    /// The chunk's normals as an image (ChunkMesh::normalGrid), which the
    /// ground is lit from at every level.
    TextureRef normalGrid;

    // Material descriptor set (set 1: 7 samplers, params UBO, 4 normal maps)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    // Per-chunk params UBO (hasLayer1/2/3)
    /// Its TerrainParamsUBO, a slot of the renderer's paramsSlots_.
    UniformSlotPool::Slot paramsSlot;

    // The chunk's textures, held: what its material set names stays resident
    // as long as the chunk does. The alpha maps are its own.
    TextureRef baseTexture;
    TextureRef layerTextures[3];
    TextureRef alphaTextures[3];
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

    // Where the chunk's geometry lives in a shared page (TerrainGeometryPage):
    // the page's slot in TerrainRenderer::geometryPages_, or -1 when the chunk
    // has buffers of its own (vertexBuffer, indexBuffer) because no page had
    // room and none could be made.
    int32_t geometryPage = -1;
    int32_t baseVertex = 0;
    uint32_t firstIndex = 0;
    uint32_t vertexCount = 0;
    /// Every index the chunk holds - all levels and their skirts - which is
    /// what its place in a page spans and gives back.
    uint32_t allIndexCount = 0;

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

    [[nodiscard]] bool isValid() const {
        return geometryPage >= 0 || (vertexBuffer != VK_NULL_HANDLE && indexBuffer != VK_NULL_HANDLE);
    }
};

/// One device-local vertex buffer and index buffer that many chunks' geometry
/// is copied into, each chunk at ranges handed out by the allocators. A
/// chunk's ranges come back after the frames that may still draw it are done
/// (deferAfterAllFrameFences), which is why the page is shared with those
/// deferred frees rather than owned outright: a free may land after the
/// renderer let the page go, and then it is only bookkeeping.
struct TerrainGeometryPage {
    AllocatedBuffer vertices;
    AllocatedBuffer indices;
    RangeAllocator vertexRanges;
    RangeAllocator indexRanges;
};

/// How the resident terrain's geometry is held, for the frame profile.
struct TerrainGeometryStats {
    uint32_t pages = 0;
    uint64_t vertexCapacity = 0;
    uint64_t verticesUsed = 0;
    uint64_t indexCapacity = 0;
    uint64_t indicesUsed = 0;
    /// Chunks with buffers of their own because no page could take them.
    uint32_t ownBufferChunks = 0;
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

    void uploadPreloadedTextures(const pipeline::PreparedTextures& textures);

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
    struct ChunkDraw {
        uint64_t surfaceIdentity = 0;
        VkDescriptorSet material = VK_NULL_HANDLE;
        GeometryDraw geometry;
        std::array<TerrainChunkGPU::LodRange, 5> ranges{};
        TerrainSurfacePush surface;
        LocalLightSet lights = kNoLocalLights;
    };
    using DepthDraw = TerrainDepthChunk<GeometryDraw>;
    using DepthTile = TerrainDepthTile<GeometryDraw>;
    struct BakeDraw {
        GeometryDraw geometry;
        VkDescriptorSet material = VK_NULL_HANDLE;
        int tileX = 0, tileY = 0, chunkX = 0, chunkY = 0;
        TerrainBake::Atlas* atlas = nullptr;
        std::shared_ptr<std::atomic<bool>> complete;
    };
    struct DrawList {
        std::vector<ChunkDraw> view;
        std::vector<std::shared_ptr<const DepthTile>> depth;
        std::vector<BakeDraw> bake;
        std::vector<std::shared_ptr<TerrainBake::Atlas>> atlases;
        bool wireframe = false;
        int culled = 0, baked = 0, crossfade = 0;
        float furthestSq = 0;
    };
    DrawList extract(const Camera& camera, const std::vector<LocalLight>& lights);
    void recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list);
    void prepareSurface(SurfacePass& pass);
    void recordSurface(VkDescriptorSet perFrameSet, const DrawList& list, const glm::vec3& camera);
    void recordDepth(VkCommandBuffer cmd, const glm::mat4& matrix, const glm::vec3& centre,
                     float radius, const DrawList& list);

    /// Outside any rendering scope, before the frame's terrain is drawn: the
    /// atlases made since the last frame readied, and the chunks waiting for
    /// their bake drawn into them, a budget a frame (TerrainBake).
    void recordBake(VkCommandBuffer cmd, const DrawList& list);
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

    [[nodiscard]] TerrainGeometryStats geometryStats() const;

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
    [[nodiscard]] int getTriangleCount() const;

    /// The height in pixels of the image the terrain is drawn into, which the
    /// level of detail is chosen against: a chunk is drawn at the coarsest
    /// level whose error stays within kLodMaxErrorPixels on screen.
    void setLodViewportHeight(uint32_t pixels) { lodViewportHeight_ = pixels; }
    pipeline::TerrainCoverage detailedCoverage() const;
    [[nodiscard]] VkContext* getVkContext() const { return vkCtx; }

    /// Where chunks register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }

private:

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
    TextureRef createAlphaTexture(const std::vector<uint8_t>& alphaData);
    /// The chunk's normal image; the flat normal when it has none, which the
    /// log says once.
    TextureRef createNormalGridTexture(const std::vector<uint8_t>& normalGrid);
    bool isChunkVisible(const TerrainChunkGPU& chunk, const Frustum& frustum);
    void calculateBoundingSphere(TerrainChunkGPU& chunk, const pipeline::ChunkMesh& meshChunk);
    VkDescriptorSet allocateMaterialSet();
    /// False when there is nothing sampleable to write, in which case the
    /// caller must drop the chunk: a descriptor set that was allocated and
    /// never written is as undefined to bind as one holding a null view.
    bool writeMaterialDescriptors(VkDescriptorSet set, const TerrainChunkGPU& chunk);
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

    /// Allocate and fill a chunk's params UBO. False means the allocation
    /// failed, and the two callers answer that differently: the one-shot load
    /// skips the chunk, the incremental one hands it back to be retried.
    bool createChunkParamsUBO(TerrainChunkGPU& gpuChunk);

    VkContext* vkCtx = nullptr;
    RtScene* rtScene_ = nullptr;
    void registerRtChunk(TerrainChunkGPU& gpuChunk, const pipeline::ChunkMesh& chunk);
    pipeline::AssetManager* assetManager = nullptr;

    // Main pipelines
    SurfacePass* surfacePass_ = nullptr;
    VkRenderPass surfaceRenderPass_ = VK_NULL_HANDLE;
    VkPipeline surfacePipeline_ = VK_NULL_HANDLE;
    void recordChunks(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const DrawList& list,
                      SurfacePass* surfaces, const glm::vec3& camera);
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline wireframePipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;

    // Shadow pipeline
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    // Descriptor pool for material sets. One set per terrain chunk, 256 chunks
    // to a tile, so this is a tile budget: 65536 covered 256 tiles, and tiles
    // are held to the *unload* radius, not the load radius - 8 loading and 11
    // unloading is 23×23 = 529 resident. Every tile past the 256th arrived
    // with no descriptor set and, before the retry in loadTerrainIncremental,
    // was dropped for good: terrain that simply was not there, in whichever
    // direction the player had travelled furthest.
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    /// Every chunk's TerrainParamsUBO (uniform_slot_pool.hpp).
    std::shared_ptr<UniformSlotPool> paramsSlots_;
    static constexpr uint32_t MAX_MATERIAL_SETS = 160 * 1024;  // 640 tiles

    // Loaded terrain chunks
    std::vector<TerrainChunkGPU> chunks;

    /// Tileset paths that would not load, said once each.
    std::unordered_set<std::string> loggedTextureLoadFails_;

    // Fallback textures, held in the shared cache like any other.
    TextureRef whiteTexture;
    TextureRef opaqueAlphaTexture;
    TextureRef flatNormalTexture;
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
    /// Chunks waiting for their bake, by their place on the map (chunkAt_).
    std::vector<uint64_t> bakeQueue_;
    int bakedDrawnChunks_ = 0;
    int crossfadeDrawnChunks_ = 0;
    /// Enqueue a chunk just added for its bake.
    void queueBake(const TerrainChunkGPU& chunk);
    /// Puts a chunk just made at the end of `chunks`, records its place in
    /// chunkAt_ and its tile's list, and queues its bake.
    void addChunk(TerrainChunkGPU&& chunk);
    /// Where each chunk is in `chunks`, by its place on the map, so a chunk
    /// can ask its neighbours' levels. Kept up as chunks come and go: rebuilt
    /// from all of them whenever the list changed, it was 30,000-40,000
    /// inserts on every frame a tile streamed in or out.
    std::unordered_map<uint64_t, uint32_t> chunkAt_;
    /// The chunks of each tile and a sphere round all of theirs: the shadow
    /// pass tests a tile before its chunks, and a tile outside the map has no
    /// chunk inside it. Kept up with chunkAt_.
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
    int renderedChunks = 0;
    float furthestDrawnSq_ = 0.0f;
    int culledChunks = 0;

    // The chunks' geometry, in pages each holding many tiles, so the draw
    // loops bind a vertex and an index buffer per page rather than per chunk.
    // Made when the resident tiles need room and let go when a page empties,
    // keeping one spare; a slot let go is null, so a chunk's page index stays
    // put. A tile is 256 chunks, each with every level of detail and its
    // skirts (pipeline::kChunkMaxVertices, kChunkMaxIndices).
    std::vector<std::shared_ptr<TerrainGeometryPage>> geometryPages_;
    static constexpr uint32_t kTilesPerGeometryPage = 16;
    static constexpr uint32_t kPageVertices = kTilesPerGeometryPage * 256 * pipeline::kChunkMaxVertices;
    static constexpr uint32_t kPageIndices = kTilesPerGeometryPage * 256 * pipeline::kChunkMaxIndices;
    /// Set, and said once, when a page could not be made: no other is tried
    /// until a page is let go or the terrain cleared, so a full device is not
    /// asked again for every chunk.
    bool geometryPagesExhausted_ = false;
    uint32_t ownBufferChunks_ = 0;

    /// Copy the chunk's geometry into a page with room, making one if none
    /// has; false when no page could be made.
    bool placeInGeometryPage(TerrainChunkGPU& gpuChunk, const std::vector<pipeline::TerrainVertex>& vertices,
                             const std::vector<pipeline::TerrainIndex>& indices);
    /// Let go of pages nothing is in any more, keeping one spare.
    void releaseEmptyGeometryPages();
    void destroyGeometryPages();
    /// Binds the page's buffers.

};

} // namespace rendering
} // namespace wowee
