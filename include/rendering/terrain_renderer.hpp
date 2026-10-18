#pragma once

#include <cmath>

#include "rendering/texture_cache.hpp"
#include "rendering/normal_map_encoding.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/local_lights.hpp"

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

/**
 * GPU-side terrain chunk data (Vulkan)
 */
struct TerrainChunkGPU {
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    /// Level 0, the full mesh, at the start of the index buffer: what the
    /// shadow pass and anything else that wants the true ground draws.
    uint32_t indexCount = 0;
    /// Each level of detail's range of the index buffer, level 0 first; a
    /// level with no indices is not offered and the next finer one is drawn.
    struct LodRange {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::array<LodRange, pipeline::kTerrainLodLevels> lods{};

    // Material descriptor set (set 1: 7 samplers, params UBO, 4 normal maps)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    // Per-chunk params UBO (hasLayer1/2/3)
    ::VkBuffer paramsUBO = VK_NULL_HANDLE;
    VmaAllocation paramsAlloc = VK_NULL_HANDLE;

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

    // Bounding sphere for frustum culling
    float boundingSphereRadius = 0.0f;
    glm::vec3 boundingSphereCenter = glm::vec3(0.0f);

    // Offsets into mega buffers for indirect drawing (-1 = not in mega buffer)
    int32_t megaBaseVertex = -1;
    uint32_t megaFirstIndex = 0;
    uint32_t vertexCount = 0;

    // The chunk as the ray traced lighting sees it (RtScene ids, or ~0u).
    uint32_t rtMesh = ~0u;
    uint32_t rtInstance = ~0u;

    [[nodiscard]] bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexBuffer != VK_NULL_HANDLE; }
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
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

    /**
     * Initialize terrain shadow pipeline (must be called after initialize()).
     * @param shadowRenderPass  Depth-only render pass used for the shadow map.
     */
    bool initializeShadow(VkRenderPass shadowRenderPass);

    /**
     * Render terrain into the shadow depth map.
     * @param cmd               Command buffer (inside shadow render pass).
     * @param lightSpaceMatrix  Orthographic light-space transform.
     * @param shadowCenter      World-space centre of shadow coverage.
     * @param shadowRadius      Cull radius around shadowCenter.
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter, float shadowRadius);

    [[nodiscard]] bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    void clear();

    void recreatePipelines();
    /// The fill pipeline and its wireframe derivative, which
    /// initialize() and recreatePipelines() both need.
    bool buildMainPassPipelines(VkDevice device,
                                wowee::rendering::VkShaderModule& vertShader,
                                wowee::rendering::VkShaderModule& fragShader);

    void setWireframe(bool enabled) { wireframe = enabled; }
    /// The frame's point lights, which every chunk picks its nearest from.
    void setFrameLocalLights(const std::vector<LocalLight>* lights) { frameLights_ = lights; }
    void setFrustumCulling(bool enabled) { frustumCullingEnabled = enabled; }
    void setFogEnabled(bool enabled) { fogEnabled = enabled; }
    [[nodiscard]] bool isFogEnabled() const { return fogEnabled; }
    void setViewDistance(float distance) { maxViewDistance_ = std::clamp(distance, 400.0f, 2400.0f); }
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
    /// level of detail is chosen against: a chunk drops a level where the
    /// finer one's triangles would be smaller than kLodMinTrianglePixels.
    void setLodViewportHeight(uint32_t pixels) { lodViewportHeight_ = pixels; }
    /// The level of detail a chunk this far from a camera with this vertical
    /// focal length (in pixels) is drawn at, before falling back over levels
    /// the chunk does not offer. Level 0 throughout when LOD is off.
    [[nodiscard]] int lodLevelAt(float distance, float focalPixels) const;
    [[nodiscard]] VkContext* getVkContext() const { return vkCtx; }

    /// Where chunks register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }

private:
    const std::vector<LocalLight>* frameLights_ = nullptr;
    TerrainChunkGPU uploadChunk(const pipeline::ChunkMesh& chunk);
    /// The tileset texture from the shared cache, loading it if it is not
    /// there; white when it cannot be had.
    TextureRef loadTexture(const std::string& path);
    /// The tileset texture's relief from the shared cache. Normally the
    /// loading worker made it; when the cache let it go since, it is derived
    /// again here, on this thread. Empty when it cannot be had.
    TextureRef loadNormalMap(const std::string& path);
    TextureRef createAlphaTexture(const std::vector<uint8_t>& alphaData);
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
    uint32_t lodViewportHeight_ = 0;
    uint64_t renderedTriangles_ = 0;
    int renderedChunks = 0;
    float furthestDrawnSq_ = 0.0f;
    int culledChunks = 0;

    // Mega vertex/index buffers for indirect drawing
    // All terrain chunks share a single VB + IB, eliminating per-chunk rebinds.
    // Indirect draw commands are built CPU-side each frame for visible chunks.
    VkBuffer megaVB_ = VK_NULL_HANDLE;
    VmaAllocation megaVBAlloc_ = VK_NULL_HANDLE;
    void* megaVBMapped_ = nullptr;
    VkBuffer megaIB_ = VK_NULL_HANDLE;
    VmaAllocation megaIBAlloc_ = VK_NULL_HANDLE;
    void* megaIBMapped_ = nullptr;
    uint32_t megaVBUsed_ = 0;  // vertices used
    uint32_t megaIBUsed_ = 0;  // indices used
    static constexpr uint32_t MEGA_VB_MAX_VERTS   = 1536 * 1024; // ~1.5M verts × 44B ≈ 64MB
    // Every level of detail of a chunk shares its vertices and adds indices:
    // 1608 a chunk against level 0's 768, so as many chunks fit as the
    // vertex buffer holds (~8600 at 181 vertices each).
    static constexpr uint32_t MEGA_IB_MAX_INDICES  = 14 * 1024 * 1024; // 14M indices × 4B = 56MB

    VkBuffer indirectBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indirectAlloc_ = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_INDIRECT_DRAWS = 8192;
};

} // namespace rendering
} // namespace wowee
