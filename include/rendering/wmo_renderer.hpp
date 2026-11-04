#pragma once

#include "rendering/wmo_draw_layer.hpp"
#include "rendering/rt_instance_changes.hpp"
#include "rendering/frame_pool.hpp"

#include "collision/wmo_shape.hpp"
#include "rendering/view_distance.hpp"

#include "rendering/normal_map_encoding.hpp"
#include "rendering/texture_cache.hpp"
#include "rendering/texture_loads.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/local_lights.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/wmo_lighting.hpp"
#include "rendering/wmo_minimap.hpp"
#include "rendering/gpu_row_store.hpp"
#include "rendering/gpu_slot_table.hpp"
#include "rendering/texture_table.hpp"
#include "rendering/geometry_pool.hpp"
#include "rendering/wmo_draws.hpp"

#include "pipeline/blp_loader.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <limits>
#include <future>
#include <algorithm>

namespace wowee {
namespace pipeline {
    struct WMOModel;
    struct WMOGroup;
    class AssetManager;
}
namespace collision { class CollisionWorld; }

namespace rendering {

class Camera;
class Frustum;
struct WaterReflectionView;
class M2Renderer;
class RtScene;
class VkContext;
class VkTexture;
struct WMOVertex;
/// A WMO model's CPU-side work, done before its upload (WMORenderer::prepareModel).
struct WmoModelPrep;
/// Its ray-traced mesh, waiting for its textures.
struct WmoPendingRt;

/**
 * WMO (World Model Object) Renderer (Vulkan)
 *
 * Renders buildings, dungeons, and large structures from WMO files.
 * Features:
 * - Multi-material rendering
 * - Batched rendering per group
 * - Frustum culling
 * - Portal visibility (future)
 */
class WMORenderer {
    struct InstanceState;
public:
    /// The owning scene holds this lease. Geometry/texture caches stay here;
    /// instances, collision indexes and incomplete uploads die with the lease.
    class InstanceSession {
    public:
        explicit InstanceSession(WMORenderer& renderer);
        ~InstanceSession();
        InstanceSession(const InstanceSession&) = delete;
        InstanceSession& operator=(const InstanceSession&) = delete;
    private:
        WMORenderer& renderer_;
        std::unique_ptr<InstanceState> state_;
    };
    WMORenderer();
    ~WMORenderer();

    /**
     * Initialize renderer (Vulkan)
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for set 0 (per-frame UBO)
     * @param assetManager Asset manager for loading textures (optional)
     */
    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assetManager = nullptr);

    /**
     * Cleanup GPU resources
     */
    void shutdown();

    /**
     * Set M2 renderer for hierarchical transform updates (doodads follow parent WMO)
     */
    void setM2Renderer(M2Renderer* renderer) { m2Renderer_ = renderer; }

    /// Where loaded models register their geometry for the ray traced lighting.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }
    /// Flush added, moved and hidden placements after producer mutations.
    /// Unchanged instances are not visited; removals retire immediately.
    void syncRtScene();

    /**
     * Load WMO model and create GPU resources
     * @param model WMO model with geometry data
     * @param id Unique identifier for this WMO instance
     * @return True if successful
     */
    bool loadModel(const pipeline::WMOModel& model, uint32_t id);

    enum class ModelLoadResult { Complete, InProgress, Failed };

    /// Upload a model a few groups at a time. InProgress means call again with
    /// the same arguments; a single large model can otherwise take over a
    /// hundred milliseconds, which lands as a visible hitch when terrain
    /// streaming finalises a tile. Prepares the model here (prepareModel) on
    /// the first call; a caller with a thread of its own prepares it there and
    /// calls loadPreparedIncremental instead.
    ModelLoadResult loadModelIncremental(const pipeline::WMOModel& model, uint32_t id, float budgetMs);

    /// Everything a model's upload works out on the CPU, done ahead of it:
    /// its vertices with their tangents, each group's collision geometry and
    /// grid, which batches draw together, its portals and doodad placements,
    /// and the mesh it adds to the ray-traced scene. Reads only the model and
    /// touches nothing of a renderer, so any thread may call it - the terrain
    /// workers do, beside parsing the file. On the main thread this was most
    /// of a large model's upload: 8-10 ms a step and 10 ms more to finish.
    [[nodiscard]] static std::shared_ptr<WmoModelPrep> prepareModel(const pipeline::WMOModel& model);

    /// loadModelIncremental for a prepared model: what is left is uploading,
    /// binding textures and making materials, a group at a time under the
    /// budget. Takes the prepared data as it uploads it, so pass the same
    /// preparation to every call for one model.
    ModelLoadResult loadPreparedIncremental(WmoModelPrep& prep, uint32_t id, float budgetMs);

    /**
     * Check if a WMO model is currently resident in the renderer
     * @param id WMO model identifier
     */
    bool isModelLoaded(uint32_t id) const;

    /// Where every instance is placed, moved, hidden and removed as it is
    /// here, with its model's shape, so the world it is solid in always has
    /// what this draws.
    void setCollisionWorld(collision::CollisionWorld* world) { collision_ = world; }

    /**
     * Check if a WMO instance is still live in the renderer. Owners that cache
     * instance IDs use this to detect an instance dropped underneath them
     * (e.g. by a renderer-wide clear) instead of addressing a dead handle.
     * @param instanceId Instance identifier returned by createInstance
     */
    bool hasInstance(uint32_t instanceId) const;

    /**
     * Unload WMO model and free GPU resources
     * @param id WMO model identifier
     */
    void unloadModel(uint32_t id);

    /**
     * Create a WMO instance in the world
     * @param modelId WMO model to instantiate
     * @param position World position
     * @param rotation Rotation (euler angles in radians)
     * @param scale Uniform scale
     * @return Instance ID
     */
    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                           const glm::vec3& rotation = glm::vec3(0.0f),
                           float scale = 1.0f, uint32_t reservedId = 0, uint16_t nameSet = 0);

    /// A placed building as the minimap draws it. Nothing for an instance
    /// that is not here or a model with no minimap description.
    [[nodiscard]] std::optional<WmoMinimapPlacement> minimapPlacement(uint32_t instanceId) const;
    /// An id no instance has, for one SceneCommands will create at the apply
    /// point (createInstance's reservedId): the caller has it now, the
    /// instance from then on.
    uint32_t reserveInstanceId() { return nextInstanceId++; }

    /**
     * Update the full transform of an existing instance (for moving transports)
     * @param instanceId Instance to update
     * @param transform World transform matrix
     */
    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);

    /// Mark an instance as a moving transport. Its collision still answers the
    /// static-world floor query (you walk onto a hull, you stand on a lift), but
    /// only when the deck is genuinely underfoot - see
    /// CollisionWorld::setWmoTransport. Idempotent; safe on every register.
    void setInstanceIsTransport(uint32_t instanceId, bool isTransport);

    /// Take an instance out of the world without unloading it.
    ///
    /// A cross-continent transport is one object on a route that spans two
    /// maps: for half its cycle the Undercity zeppelin is over Howling Fjord
    /// and is not here at all. Its hull holds still on this map's slice while
    /// it is away, so without this it would sit invisible-but-solid over the
    /// tower, and a player could stand on a zeppelin that is on another
    /// continent. Hidden instances are skipped by drawing and taken out of the
    /// collision world. Its child doodads
    /// (addDoodadToInstance) are hidden and shown with it, in the M2 renderer.
    void setInstanceHidden(uint32_t instanceId, bool hidden);

    /**
     * Add doodad (child M2) to WMO instance
     * @param instanceId WMO instance to add doodad to
     * @param m2InstanceId M2 instance ID of the doodad
     * @param localTransform Local transform relative to WMO origin
     */
    void addDoodadToInstance(uint32_t instanceId, uint32_t m2InstanceId, const glm::mat4& localTransform);

    // Forward declare DoodadTemplate for public API
    struct DoodadTemplate {
        std::string m2Path;
        glm::mat4 localTransform;
    };

    /**
     * Get doodad templates for a WMO model
     * @param modelId WMO model ID
     * @return Vector of doodad templates (empty if no doodads or model not found)
     */
    /// Set an animation on every child doodad of an instance. Returns how many
    /// doodads were addressed, so a caller streaming them in over several frames
    /// can tell when the set has grown and re-apply. A doodad lacking the
    /// sequence is left as it is.
    size_t setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId, bool loop);
    /// How many child doodads an instance has (addDoodadToInstance).
    [[nodiscard]] size_t instanceDoodadCount(uint32_t instanceId) const;
    const std::vector<DoodadTemplate>* getDoodadTemplates(uint32_t modelId) const;

    /**
     * Remove WMO instance
     * @param instanceId Instance to remove
     */
    void removeInstance(uint32_t instanceId);
    /**
     * Remove multiple WMO instances with a single spatial-index rebuild.
     */
    void removeInstances(const std::vector<uint32_t>& instanceIds);

    /**
     * Remove all instances
     */
    void clearInstances();

    /**
     * Clear all instances, loaded models, and texture cache (for map transitions)
     */
    void clearAll();

    /**
     * Render all WMO instances (Vulkan)
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0)
     * @param camera Camera for frustum culling
     */
    struct Frame;
    /// What the GPU's cull of this frame needs (wmo_draws.hpp): the rows of
    /// the instances, groups and materials changed since a frame known to be
    /// on the GPU, the camera's view and, when there is water to reflect in,
    /// what its `mirror` can show. The portal walk, when it runs, goes from
    /// the camera's and the `viewer`'s groups.
    std::shared_ptr<const Frame> extract(const Camera& camera, const glm::vec3& viewer,
                                        const WaterReflectionView* mirror = nullptr);
    void useFrame(std::shared_ptr<const Frame> frame) { recording_ = std::move(frame); }
    /// Record into `cmd`, outside a render pass and before any of the
    /// frame's WMO draws, the rows the frame carries and the cull of its
    /// colour lists, picking each drawn group's lights from the frame's
    /// `lights`.
    void prepareDraws(VkCommandBuffer cmd, uint32_t frameIndex, ::VkBuffer lights, uint32_t lightCount);
    /// The frame whose rows prepareDraws recorded is submitted: they are on
    /// the GPU.
    void frameSubmitted();
    /// The passes that draw the buildings in colour: each its own list,
    /// which the cull wrote. The water's reflection is recorded on the main
    /// thread and the view on a frame worker.
    enum class Pass : uint8_t { View, Reflection, Count };
    /// Which of a pass's batches a call records; see WmoDrawLayer.
    using Layer = WmoDrawLayer;

    /// Records the pass's list, a call a bucket of the layer's kinds: it
    /// reads nothing of the scene.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, Pass pass = Pass::View,
                Layer layer = Layer::All);

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);

    /// The cull of a shadow map round `centre`, outside a render pass: what
    /// casts into it is a group whose box comes within `radius` of it, or
    /// 180 yards for a smaller map - a distant building's shadow reaches the
    /// player. The map's number for renderShadow, or nothing when no
    /// building can cast into it this frame.
    std::optional<uint32_t> cullShadowMap(VkCommandBuffer cmd, const glm::vec3& centre, float radius);
    /// The buildings' side of a shadow map the cull made.
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, uint32_t map);

    /**
     * Get number of loaded models
     */
    void recreatePipelines();
    /// The three main-pass pipelines, which initialize() and
    /// recreatePipelines() both need and each used to describe.
    bool buildMainPassPipelines(VkDevice device,
                                wowee::rendering::VkShaderModule& vertShader,
                                wowee::rendering::VkShaderModule& fragShader);
    bool isInitialized() const { return initialized_; }
    uint32_t getModelCount() const { return loadedModels.size(); }

    /**
     * Get number of active instances
     */
    uint32_t getInstanceCount() const { return scene_->instances.size(); }
    size_t getLoadedModelCount() const { return loadedModels.size(); }

    /**
     * Remove models that have no instances referencing them
     * Call periodically to free GPU memory
     */
    void cleanupUnusedModels();

    /**
     * Get total triangle count (all instances)
     */
    uint32_t getTotalTriangleCount() const;

    /**
     * Get total draw call count (last frame)
     */
    /// The draws the GPU's cull made of the frames that finished last: the
    /// view's, its blended layer's, and every shadow map's.
    uint32_t getDrawCallCount() const { return drawPass_.counted().viewDraws + drawPass_.counted().blendedDraws; }
    uint32_t getBlendedDrawCallCount() const { return drawPass_.counted().blendedDraws; }
    uint32_t getShadowDrawCallCount() const { return drawPass_.counted().casterDraws; }
    /// The groups the view drew and those it left out, by why.
    [[nodiscard]] const WmoCullStatsGPU& cullCounts() const { return drawPass_.counted(); }

    /**
     * Normal mapping / Parallax Occlusion Mapping settings
     */
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; materialSettingsDirty_ = true; }
    void setNormalMapStrength(float s) { normalMapStrength_ = s; materialSettingsDirty_ = true; }
    void setPOMEnabled(bool enabled) { pomEnabled_ = enabled; materialSettingsDirty_ = true; }
    void setPOMQuality(int q) { pomQuality_ = q; materialSettingsDirty_ = true; }
    bool isNormalMappingEnabled() const { return normalMappingEnabled_; }
    float getNormalMapStrength() const { return normalMapStrength_; }
    bool isPOMEnabled() const { return pomEnabled_; }
    int getPOMQuality() const { return pomQuality_; }

    /**
     * Enable/disable wireframe rendering
     */
    void setWireframeMode(bool enabled) { wireframeMode = enabled; }

    /**
     * Enable/disable frustum culling
     */
    void setFrustumCulling(bool enabled) { frustumCulling = enabled; }

    /**
     * Enable/disable portal-based visibility culling
     */
    void setPortalCulling(bool enabled) { portalCulling = enabled; }
    /// The frame's point lights, which every group picks its nearest from.
    bool isPortalCullingEnabled() const { return portalCulling; }

    /**
     * Enable/disable distance-based group culling
     */
    void setDistanceCulling(bool enabled, float maxDistance = 500.0f) {
        distanceCulling = enabled;
        maxGroupDistance = maxDistance;
        maxGroupDistanceSq = maxDistance * maxDistance;
    }
    bool isDistanceCullingEnabled() const { return distanceCulling; }
    float getMaxGroupDistance() const { return maxGroupDistance; }
    void setViewDistance(float distance) { viewDistance_ = std::clamp(distance, kMinViewDistance, kMaxViewDistance); }

    /**
     * Enable/disable GPU occlusion query culling (stubbed in Vulkan)
     */
    void setOcclusionCulling(bool /*enabled*/) { /* stubbed */ }
    bool isOcclusionCullingEnabled() const { return false; }

    /**
     * Get number of groups culled by occlusion queries last frame
     */
    uint32_t getOcclusionCulledGroups() const { return 0; }

    // Lighting/fog/shadow are now in the per-frame UBO; these are no-ops for API compat
    void setFog(const glm::vec3& /*color*/, float /*start*/, float /*end*/) {}
    void setLighting(const float /*lightDir*/[3], const float /*lightColor*/[3],
                     const float /*ambientColor*/[3]) {}
    void setShadowMap(uint32_t /*depthTex*/, const glm::mat4& /*lightSpace*/) {}
    void clearShadowMap() {}

    /// Changes whenever what is drawn does: an instance created or removed, a
    /// model finished loading or unloaded.
    [[nodiscard]] uint64_t contentRevision() const { return contentRevision_; }

    /// A building texture by the name a model gives it: from the shared
    /// cache, or started on a worker of `loads`, which tries the names it may
    /// be filed under and, given `normals`, makes its normal/height map in
    /// that format from the same read. Any thread: a terrain worker asks for
    /// a tile's ahead of the tile's finalize, which then finds them made.
    static TextureLoads::State requestTexture(TextureLoads& loads, const std::string& path,
                                              std::optional<NormalMapFormat> normals, TextureRef& out);
    /// A building texture's normal/height map from its RGBA8 pixels, encoded
    /// as `format` with its mip chain: what a worker prepares for the upload.
    static EncodedNormalMap prepareNormalHeightMap(const uint8_t* pixels, uint32_t width, uint32_t height,
                                                   NormalMapFormat format);
    /// How this device keeps them (normalMapFormatFor).
    [[nodiscard]] NormalMapFormat normalMapFormat() const;

    /// Where the building textures and their normal/height maps are made:
    /// the renderer's one loader, which outlives this.
    void setTextureLoads(TextureLoads* loads) { textureLoads_ = loads; }
    /// Asks again for the textures buildings are drawn without - failed, or
    /// still being made - and gives the batches theirs. Once a frame, on the
    /// main thread.
    void retryPendingTextures();
    /// Models placed and not drawn yet, waiting for a texture still being
    /// made.
    [[nodiscard]] size_t modelsAwaitingTextures() const;
    /// The textures those models wait for, as "model <id>: <texture>", the
    /// first `limit` of them: what a wait for them says.
    [[nodiscard]] std::vector<std::string> awaitedTextures(size_t limit) const;

private:
    // Holds a model's group resources before their upload.
    friend struct WmoModelPrep;

    /// A batch's row of the material table (WmoMaterial in wmo_material.glsli,
    /// std430): what its draws used to bind as a set of their own - the
    /// parameters and the two textures - and the cloth they pushed.
    struct WmoMaterialGPU {
        TextureHandle texture = kNoTextureHandle;       // diffuse
        TextureHandle normalHeight = kNoTextureHandle;  // normal + height map
        int32_t hasTexture = 0;
        int32_t alphaTest = 0;
        int32_t light = 0;               // WmoLight the batch is drawn with
        int32_t transitionLight = -1;    // WmoLight under it by 1 - MOCV alpha, -1 for none
        float specularIntensity = 0.0f;
        int32_t enableNormalMap = 0;
        int32_t enablePOM = 0;
        float pomScale = 0.0f;           // height scale
        int32_t pomMaxSamples = 0;       // most ray-march steps
        float heightMapVariance = 0.0f;  // low variance = no POM
        float normalMapStrength = 0.0f;  // 0 flat, 1 full, 2 exaggerated
        int32_t isLava = 0;              // lava/magma UV scroll
        int32_t emissive = 0;            // 0 none, 1 lamp glass, 2 firelit
        int32_t hasVertexColors = 0;     // the group carries MOCV (MOGP flag 0x4)
        int32_t receivesExteriorShadows = 0;  // wmoReceivesExteriorShadows
        int32_t pad[3] = {};
        glm::vec4 ambient{0.0f};         // rgb: the model's interior ambient
        glm::vec4 sidn{0.0f};            // rgb: F_SIDN glow colour, black without the flag
        glm::vec4 cloth{0.0f};           // top z, drop, centre x, y; zero drop for no cloth
    };
    static_assert(sizeof(WmoMaterialGPU) == 128, "wmo_material.glsli's WmoMaterial");

    /**
     * WMO group GPU resources
     */
    struct GroupResources {
        /// Where its vertices and indices live (geometry_); unplaced, it is
        /// not drawn.
        GeometryPool::Range geometry;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;

        uint32_t groupFlags = 0;
        bool allUntextured = false;  // True if ALL batches use fallback white texture (collision/placeholder group)
        bool isLOD = false;          // Distance-only group (skip when camera is close)
        /// How far a banner hung in the group can swing past its box, in the
        /// building's units; zero when it has none (clothSwayReach).
        float swayReach = 0.0f;

        // Material batches (start index, count, material ID)
        struct Batch {
            uint32_t startIndex;   // First index in EBO
            uint32_t indexCount;   // Number of indices to draw
            uint8_t materialId;    // Material/texture reference
            /// The MOBA run it is in, which picks its light with its material.
            pipeline::WMOBatchKind kind = pipeline::WMOBatchKind::Exterior;
        };
        std::vector<Batch> batches;

        // Pre-merged batches for efficient rendering (computed at load time)
        struct MergedBatch {
            VkTexture* texture = nullptr;   // from cache, NOT owned
            /// The model texture this batch wanted and did not get, drawn white
            /// until retryPendingTextures loads it. UINT32_MAX when it has it.
            uint32_t pendingTextureIndex = UINT32_MAX;
            VkTexture* normalHeightMap = nullptr;  // generated from diffuse, NOT owned
            float heightMapVariance = 0.0f; // variance of height map (low = flat texture)
            /// Its textures' places in the texture table; the material row
            /// carries them.
            TextureHandle textureHandle = kNoTextureHandle;
            TextureHandle normalHandle = kNoTextureHandle;
            /// Its row of the material table (WmoMaterialGPU), which its draws
            /// name; none when its textures have no place, and it is not drawn.
            uint32_t materialRow = UINT32_MAX;
            bool hasTexture = false;
            bool alphaTest = false;
            bool isTransparent = false;     // blendMode >= 2
            bool twoSided = false;          // F_UNCULLED: back faces shown too
            uint32_t sidnColor = 0;         // F_SIDN glow colour (BGRA), 0 without
            bool isLava = false;            // lava/magma texture (UV scroll)
            /// Cloth hung in the building's own mesh: where the bar is, how
            /// far the cloth drops below it, and where it hangs.
            ///
            /// Stormwind's gate banners are geometry in the city WMO rather
            /// than doodads - the material is STORMWINDBANNER01 - so the sway
            /// a banner model gets has to be applied to the batch that draws
            /// one. Zero drop means the batch is not cloth.
            float clothTop = 0.0f;
            float clothDrop = 0.0f;
            float clothCentreX = 0.0f;
            float clothCentreY = 0.0f;
            // 0 = not luminous, 1 = authored lamp glass (bright), 2 = firelit
            // from behind with a flicker (clock faces). See wmo.frag's emissive
            // branch for how each level is shaded.
            uint8_t emissiveLevel = 0;
            WmoBatchLighting lighting;      // the light it is drawn with (wmo_lighting.hpp)
            // For multi-draw: store index ranges
            struct DrawRange { uint32_t firstIndex; uint32_t indexCount; };
            std::vector<DrawRange> draws;
        };
        std::vector<MergedBatch> mergedBatches;

        /// The group's triangles as the collision world and the ray-traced
        /// mesh have them: shared, built on the worker that prepared the model.
        std::shared_ptr<const collision::WmoGroupShape> shape;
    };

    /**
     * Portal data for visibility culling
     */
    struct PortalData {
        uint16_t startVertex;
        uint16_t vertexCount;
        glm::vec3 normal;
        float distance;
    };

    struct PortalRef {
        uint16_t portalIndex;
        uint16_t groupIndex;
        int16_t side;
    };

    /**
     * Loaded WMO model data
     */
    struct ModelData {
        uint32_t id;
        std::vector<GroupResources> groups;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;
        glm::vec3 wmoAmbientColor{0.5f, 0.5f, 0.5f};  // From MOHD, used for interior lighting
        bool isLowPlatform = false;
        /// Its groups and portals as the minimap reads them; null for a
        /// model with no path to name its minimap tiles by.
        std::shared_ptr<const WmoMinimapModel> minimap;
        /// Its groups' shapes, gathered for the collision world once every
        /// group is uploaded and the model is resident.
        std::shared_ptr<const collision::WmoShape> shape;

        // Doodad templates (M2 models placed in WMO, stored for instancing)
        // Uses the public DoodadTemplate struct defined above
        std::vector<DoodadTemplate> doodadTemplates;

        // The model's textures in path order, held for as long as the model
        // is: its batches and the ray-traced scene point at them.
        std::vector<TextureRef> textures;
        /// The normal/height map made from each texture, where there is one
        /// (parallel to textures).
        std::vector<TextureRef> normalMaps;
        std::vector<std::string> textureNames;  // lowercase texture paths (parallel to textures)

        // Material texture indices (materialId -> texture index)
        std::vector<uint32_t> materialTextureIndices;

        // Material blend modes (materialId -> blendMode; 1 = alpha-test cutout)
        std::vector<uint32_t> materialBlendModes;

        // Material flags (materialId -> flags; 0x01 = unlit)
        std::vector<uint32_t> materialFlags;

        // Material night glow colour (materialId -> MOMT sidnColor, BGRA),
        // drawn only where the material has F_SIDN.
        std::vector<uint32_t> materialSidnColors;

        // Portal visibility data
        std::vector<PortalData> portals;
        std::vector<glm::vec3> portalVertices;
        std::vector<PortalRef> portalRefs;
        // For each group: which portal refs belong to it (start index, count)
        std::vector<std::pair<uint16_t, uint16_t>> groupPortalRefs;

        /// Merged batches drawn white because their texture failed to load,
        /// or lost its place in the cache, as (group, merged batch) indices.
        std::vector<std::pair<uint32_t, uint32_t>> pendingTextureBatches;

        // Set once the textures and materials below have been populated, so a
        // resumed load skips straight to the groups it has left.
        bool setupDone = false;
        /// Texture slots still being made on a worker. Until none is, the
        /// model is not drawn (describeDraws), so it appears whole rather
        /// than white and then textured, and its ray-traced mesh waits in
        /// pendingRt for the colours its textures give it.
        std::vector<uint32_t> loadingSlots;
        bool texturesLoading = false;
        std::shared_ptr<WmoPendingRt> pendingRt;
        // Next group to upload. A large model's groups are spread across several
        // calls under a time budget rather than uploaded in one stall.
        size_t nextGroupIndex = 0;
        uint32_t loadedGroups = 0;

        [[nodiscard]] uint32_t getTotalTriangles() const {
            uint32_t total = 0;
            for (const auto& group : groups) {
                total += group.indexCount / 3;
            }
            return total;
        }
    };

    /**
     * WMO instance in the world
     */
    struct WMOInstance {
        uint32_t id;
        uint32_t modelId;
        glm::vec3 position;
        glm::vec3 rotation;  // Euler angles (radians)
        float scale;
        /// The placement's NameSetID: which WMOAreaTable rows name it.
        uint16_t nameSet = 0;
        glm::mat4 modelMatrix;
        glm::mat4 invModelMatrix;  // Cached inverse for collision
        glm::vec3 worldBoundsMin;
        glm::vec3 worldBoundsMax;
        std::vector<std::pair<glm::vec3, glm::vec3>> worldGroupBounds;

        // Doodad tracking: M2 instances that are children of this WMO
        struct DoodadInfo {
            uint32_t m2InstanceId;       // ID of the M2 instance
            glm::mat4 localTransform;    // Local transform relative to WMO origin
        };
        std::vector<DoodadInfo> doodads;

        /// Loaded, positioned, and not in the world right now. See
        /// setInstanceHidden.
        bool hidden = false;

        void updateModelMatrix();
    };

    /// The instance with this id, through the id index; null for none.
    WMOInstance* findInstance(uint32_t instanceId);
    const WMOInstance* findInstance(uint32_t instanceId) const;

    /// Recomputes one instance's world and per-group bounds after its
    /// model matrix has changed.
    void refreshInstanceBounds(WMOInstance& inst);

    /// A group's resources as far as the CPU takes them - bounds, collision
    /// shape, batches - and the vertices, tangents included, its
    /// buffer is to hold. False for a group with nothing to draw. Any thread.
    static bool prepareGroupResources(const pipeline::WMOGroup& group, GroupResources& resources,
                                      std::vector<WMOVertex>& vertices);

    /// Whether a model may start loading now: Complete when it is resident
    /// already, Failed when the cache is full, nothing when it may load.
    std::optional<ModelLoadResult> admitModel(uint32_t id);
    /// Preparations loadModelIncremental made itself, for models whose loads
    /// are still in progress.
    std::unordered_map<uint32_t, std::shared_ptr<WmoModelPrep>> preparedHere_;

    /**
     * Check if group is visible in frustum
     */
    bool isGroupVisible(const GroupResources& group, const glm::mat4& modelMatrix,
                       const Camera& camera) const;

    /**
     * Find which group index contains a position (model space)
     * @return Group index or -1 if outside all groups
     */
    int findContainingGroup(const ModelData& model, const glm::vec3& localPos) const;

    /**
     * Get visible groups via portal traversal
     */
    /// Seeds from both the camera's group and the character's: at a doorway the
    /// two are in different rooms, and walking from only one while judging every
    /// door against the camera's frustum empties the interior.
    void getVisibleGroupsViaPortals(const ModelData& model,
                                     const glm::vec3& cameraLocalPos,
                                     const glm::vec3& viewerLocalPos,
                                     const Frustum& frustum,
                                     const glm::mat4& modelMatrix,
                                     std::unordered_set<uint32_t>& outVisibleGroups) const;

    /**
     * Test if a portal polygon is visible from a position through a frustum
     */
    bool isPortalVisible(const ModelData& model, uint16_t portalIndex,
                         const glm::vec3& cameraLocalPos,
                         const Frustum& frustum,
                         const glm::mat4& modelMatrix) const;

    /// A building texture and the normal/height map made from it.
    struct LoadedTexture {
        /// White when the file would not load or found no room.
        TextureRef diffuse;
        /// Empty where none is made: normal mapping and parallax off, the
        /// texture white, or the map neither prepared by the terrain workers
        /// nor made here.
        TextureRef normalHeight;
    };
    /// From the shared cache, or started on a worker (TextureLoads): white,
    /// with `*loading` set, while it or its normal/height map is being made.
    LoadedTexture loadTexture(const std::string& path, bool* loading);
    /// The normal/height map of a resident texture, from the cache or made on
    /// a worker. Empty while loading (`*loading` set), and where none is
    /// wanted or it could not be made.
    TextureRef loadNormalHeightMap(const std::string& resolvedPath, bool* loading);
    /// The names a model's texture path may be filed under, most likely
    /// first; none for a path that names nothing.
    static std::vector<std::string> textureNames(const std::string& path);
    TextureLoads* textureLoads_ = nullptr;

    /// A merged batch's material row written from it as it is, and the
    /// rendering settings; placed when it has none.
    void createMergedBatchMaterial(const ModelData& model, const GroupResources& group,
                                   GroupResources::MergedBatch& mb);
    /// Point a batch at the normal/height map made from its texture, if any.
    void attachNormalHeightMap(const ModelData& model, uint32_t texIndex,
                               GroupResources::MergedBatch& mb) const;
    std::vector<uint32_t> modelsWithPendingTextures_;

    /// A group's geometry and material rows given back, after the frames in
    /// flight that may still draw them.
    void destroyGroupGPU(GroupResources& group);


    /// Files every instance's index under its id again, after the list was
    /// compacted.
    void rebuildIndexById();

    // Vulkan context
    VkContext* vkCtx_ = nullptr;

    // Asset manager for loading textures
    pipeline::AssetManager* assetManager = nullptr;

    // M2 renderer for hierarchical transforms (doodads following WMO parent)
    M2Renderer* m2Renderer_ = nullptr;

    RtScene* rtScene_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> rtModelMeshes_;  // modelId -> RtScene mesh
    /// Adds the mesh the preparation built to the ray-traced scene, its
    /// triangles coloured by the textures the model now has.
    void registerRtModel(uint32_t modelId, const ModelData& model, WmoPendingRt& source);
    void releaseRtModel(uint32_t modelId);

    collision::CollisionWorld* collision_ = nullptr;

    // Vulkan pipelines
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;     // no alpha test: the shader cannot discard it
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;  // opaque with the alpha test (lattices, foliage)
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;
    VkPipeline wireframePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering: set 0 the texture table, set 1 the frame slot's
    // caster records and the group and instance tables (wmo_shadow.vert).
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool shadowDescPool_ = VK_NULL_HANDLE;
    VkDescriptorSet shadowSet_[2] = {};
    uint32_t shadowSetVersion_[2] = {~0u, ~0u};

    /// Every merged batch's material, a row each (WmoMaterialGPU), written
    /// when a batch's material or the settings change. The rows ride in the
    /// extracted frames until one carrying them is submitted
    /// (gpu_row_store.hpp); the draws read them at set 2.
    GpuSlotTable materialTable_{sizeof(WmoMaterialGPU), "WMO material table"};
    GpuRowStore<WmoMaterialGPU> materialRows_;
    /// The frame serial every table's rows are carried by - the materials'
    /// and the layout's - and the newest on the GPU.
    uint32_t frameSerial_ = 0;
    std::atomic<uint32_t> frameApplied_{0};
    uint32_t pendingSerial_ = 0;
    /// Set 2: the material table and what the frame slot's draws read -
    /// their records, the groups' lights, the group and instance tables - a
    /// set per frame slot, rewritten when one of them is replaced.
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    VkDescriptorSet materialSet_[2] = {};
    uint32_t materialSetVersion_[2] = {};
    uint32_t materialSetDrawVersion_[2] = {~0u, ~0u};
    /// Whether this slot's draws may be recorded: its sets name the tables
    /// and buffers as they are.
    bool drawsReady_[2] = {};
    /// The device's maxDrawIndirectCount: a bucket bigger than it is drawn
    /// by several calls.
    uint32_t maxDrawsPerCall_ = 1;
    /// What the buildings are drawn from on the GPU: the rows and places as
    /// the main thread keeps them, and the cull that turns them into draws.
    WmoDrawLayout layout_;
    WmoDrawPass drawPass_;
    /// A model's groups' draws as the layout takes them.
    std::vector<WmoGroupDraws> describeDraws(const ModelData& model) const;
    /// The layout's instances caught up with the scene: placed, moved,
    /// shown, hidden, gone.
    void catchUpLayout();
    /// The portal walk's verdict for every instance the camera stands in,
    /// a bit a group row left out.
    void walkPortals(const Camera& camera, const glm::vec3& viewer, std::vector<uint32_t>& culled) const;
    /// Every material row written anew from its batch and the settings.
    void rewriteMaterialRows();
    /// Every group's vertices and indices, in shared pages.
    GeometryPool geometry_;

    /// When a path that would not load, or found no room, is worth asking for
    /// again: a failed MPQ read is often transient.
    std::unordered_set<std::string> loggedTextureLoadFails_;

    // Default white texture, held in the shared cache.
    TextureRef whiteTexture_;

    // Flat normal placeholder (128,128,255,128) = up-pointing normal, mid-height
    TextureRef flatNormalTexture_;

    // Loaded models (modelId -> ModelData)
    std::unordered_map<uint32_t, ModelData> loadedModels;
    // Models part-way through an incremental load; moved into loadedModels once
    // every group is uploaded, so a half-built model is never rendered.
    size_t modelCacheLimit_ = 4000;
    uint32_t modelLimitRejectWarnings_ = 0;

    // Active instances
    uint32_t nextInstanceId = 1;

    bool initialized_ = false;

    // Normal mapping / POM settings
    bool normalMappingEnabled_ = true;   // on by default
    float normalMapStrength_ = 0.8f;     // 0.0 = flat, 1.0 = full, 2.0 = exaggerated
    bool pomEnabled_ = true;             // on by default
    int pomQuality_ = 1;                 // 0=Low(16), 1=Medium(32), 2=High(64)
    bool materialSettingsDirty_ = false; // rewrite the material rows when settings change

    // Rendering state
    bool wireframeMode = false;
    bool frustumCulling = true;  // dead: every instance is included, the group cull does the work
    bool portalCulling = true;   // AABB transform bug fixed; conservative frustum test (no plane-side check) is visually safe
    bool distanceCulling = false;  // Disabled - causes ground to disappear

    /// Optional portal/frustum and tighter group culling stay off by default:
    /// they historically removed nearby building interiors. The common horizontal
    /// view-distance limit always applies, matching terrain even with this off.
    /// WOWEE_WMO_CULL=1 enables the additional culling for diagnostics.
    bool cullingEnabled_ = false;
    float maxGroupDistance = 500.0f;
    float maxGroupDistanceSq = 250000.0f;  // maxGroupDistance^2
    float viewDistance_ = 1200.0f;

    /// See contentRevision(). Bumped by markContentChanged().
    uint64_t contentRevision_ = 1;
    void markContentChanged() { ++contentRevision_; }

    // Parallel visibility culling
    uint32_t numCullThreads_ = 1;

    struct InstanceState {
        std::unordered_map<uint32_t, ModelData> loadingModels_;
        std::vector<WMOInstance> instances;
        /// Each instance's slot in the copies kept of this scene.
        InstanceSlots slots;
        RtInstanceChanges rtChanges;
        std::unordered_map<uint32_t, size_t> instanceIndexById;
    };
    InstanceState defaultInstances_;
    InstanceState* scene_ = &defaultInstances_;

public:
    struct Frame {
        void clear() noexcept;
        bool wireframe = false;
        /// The serial the rows this frame carries go by (gpu_row_store.hpp).
        uint32_t serial = 0;
        /// The material rows it carries.
        uint32_t materialRowCount = 0;
        std::vector<uint32_t> materialUpdateSlots;
        std::vector<WmoMaterialGPU> materialUpdateRows;
        /// The instances', groups' and ranges' rows, the buckets and the
        /// cull's view.
        WmoDrawFrame draws;
    };
private:
    FramePool<Frame> framePool_;
    std::shared_ptr<const Frame> recording_ = std::make_shared<Frame>();

};

} // namespace rendering
} // namespace wowee
