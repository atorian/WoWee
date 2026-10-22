#pragma once
#include "rendering/scene_view.hpp"

#include "rendering/rt_instance_changes.hpp"
#include "rendering/frame_pool.hpp"

#include "collision/wmo_shape.hpp"
#include "rendering/view_distance.hpp"

#include "rendering/normal_map_encoding.hpp"
#include "rendering/texture_cache.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/local_lights.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/wmo_lighting.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/uniform_slot_pool.hpp"

#include "pipeline/blp_loader.hpp"
#include "pipeline/grass_clearing.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <atomic>
#include <memory>
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
class M2Renderer;
class RtScene;
class VkContext;
class VkTexture;
struct WMOVertex;
/// A WMO model's CPU-side work, done before its upload (WMORenderer::prepareModel).
struct WmoModelPrep;

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
                           float scale = 1.0f, uint32_t reservedId = 0);
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
    /** Pre-update mutable state (frame ID, material UBOs) on main thread before parallel render. */
    struct Frame;
    std::shared_ptr<const Frame> extract(const Camera& camera, const glm::vec3& viewer,
                                        const std::vector<LocalLight>& lights, const SceneView* reflection = nullptr);
    void useFrame(std::shared_ptr<const Frame> frame) { recording_ = std::move(frame); }
    /// The passes that draw the buildings in colour. Each culls into a draw
    /// list of its own: the water's reflection is recorded on the main thread
    /// and the view on a frame worker, and they share nothing they write.
    enum class Pass : uint8_t { View, Reflection, Count };

    /// viewerPos is the character; portal culling seeds from it as well as from
    /// the camera, because at a doorway the two stand in different groups and
    /// neither alone is reliably the right place to start.
    ///
    /// Culls the scene into the pass's draw list, then records from that list
    /// alone: the recording reads no instance.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                const glm::vec3* viewerPos = nullptr, Pass pass = Pass::View);

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);

    /**
     * Render depth-only for shadow casting. The first depth-only pass of a
     * frame resets the frame slot's texture sets; a later one in the same
     * frame (the sea bed's map) passes `resetDescriptors` false and reuses
     * them, as M2Renderer::renderShadow does.
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f,
                      bool resetDescriptors = true);

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
    uint32_t getDrawCallCount() const { return viewDrawCallCount_; }
    /// Draw calls the depth-only passes recorded this frame, from the frame's
    /// first one (the sun's) on.
    uint32_t getShadowDrawCallCount() const { return shadowDrawCalls_; }

    /**
     * Normal mapping / Parallax Occlusion Mapping settings
     */
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; }
    void setNormalMapStrength(float s) { normalMapStrength_ = s; }
    void setPOMEnabled(bool enabled) { pomEnabled_ = enabled; }
    void setPOMQuality(int q) { pomQuality_ = q; }
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

    /**
     * Append the 2D footprints of instances touching the given window, for
     * the grass generator's clearings - grass grows short and sparse beside
     * a building the way it does beside a road. Per-group boxes rather than
     * the instance's outer bounds: a keep's outer box swallows its own
     * courtyards, and a courtyard is exactly where grass belongs.
     */
    void collectGrassClearings(float minX, float minY, float maxX, float maxY,
                               std::vector<pipeline::GrassClearingSource>& out) const;

    /// Changes whenever what is drawn does: an instance created or removed, a
    /// model finished loading or unloaded.
    [[nodiscard]] uint64_t contentRevision() const { return contentRevision_; }

    // Pre-decoded BLP cache: set before calling loadModel() to skip main-thread BLP decode
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

    // Normal/height maps may be made and encoded by terrain workers alongside
    // the decoded diffuse textures. loadTexture() then only performs the
    // bounded GPU upload while building the WMO model on the main thread.
    void setPredecodedNormalMapCache(std::unordered_map<std::string, EncodedNormalMap>* cache) {
        predecodedNormalMapCache_ = cache;
    }
    /// A building texture's normal/height map from its RGBA8 pixels, encoded
    /// as `format` with its mip chain: what a worker prepares for the upload.
    static EncodedNormalMap prepareNormalHeightMap(const uint8_t* pixels, uint32_t width, uint32_t height,
                                                   NormalMapFormat format);
    /// How this device keeps them (normalMapFormatFor).
    [[nodiscard]] NormalMapFormat normalMapFormat() const;

    // Defer normal/height map generation during streaming to avoid CPU stalls
    void setDeferNormalMaps(bool defer) { deferNormalMaps_ = defer; }

private:
    // Holds a model's group resources before their upload.
    friend struct WmoModelPrep;

    const std::vector<LocalLight>* frameLights_ = nullptr;
    // WMO material UBO - matches WMOMaterial in wmo.frag.glsl
    // Resource properties only. Per-frame normal/POM settings are recorded
    // separately, so queued frames never rewrite a shared material allocation.
    struct WMOMaterialUBO {
        int32_t hasTexture;
        int32_t alphaTest;
        int32_t light;
        int32_t transitionLight;
        float specularIntensity;
        float heightMapVariance;
        int32_t isLava;
        int32_t emissive;
        float wmoAmbientR;
        float wmoAmbientG;
        float wmoAmbientB;
        int32_t hasVertexColors;
        int32_t receivesExteriorShadows;
        float sidnR;
        float sidnG;
        float sidnB;
    };


    /**
     * WMO group GPU resources
     */
    struct GroupDrawData;
    struct GroupResources {
        mutable std::shared_ptr<const GroupDrawData> drawData;
        ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        ::VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;

        uint32_t groupFlags = 0;
        bool allUntextured = false;  // True if ALL batches use fallback white texture (collision/placeholder group)
        bool isLOD = false;          // Distance-only group (skip when camera is close)

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
            VkDescriptorSet materialSet = VK_NULL_HANDLE;  // set 1
            /// Its WMOMaterialUBO, a slot of materialSlots_.
            UniformSlotPool::Slot materialSlot;
            bool hasTexture = false;
            bool alphaTest = false;
            bool isTransparent = false;     // blendMode >= 2
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
    /// Immutable GPU description, shared across every instance and frame until
    /// a material binding changes. Collision meshes stay in GroupResources.
    struct GroupDrawData {
        ::VkBuffer vertexBuffer = VK_NULL_HANDLE, indexBuffer = VK_NULL_HANDLE;
        glm::vec3 boundingBoxMin{0}, boundingBoxMax{0};
        uint32_t groupFlags = 0;
        bool isLOD = false;
        std::vector<GroupResources::MergedBatch> mergedBatches;
    };

    struct ModelData {
        uint32_t id;
        std::vector<GroupResources> groups;
        glm::vec3 boundingBoxMin;
        glm::vec3 boundingBoxMax;
        glm::vec3 wmoAmbientColor{0.5f, 0.5f, 0.5f};  // From MOHD, used for interior lighting
        bool isLowPlatform = false;
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
        /// as (group, merged batch) indices.
        std::vector<std::pair<uint32_t, uint32_t>> pendingTextureBatches;

        // Set once the textures and materials below have been populated, so a
        // resumed load skips straight to the groups it has left.
        bool setupDone = false;
        // Next group to upload. A large model's groups are spread across several
        // calls under a time budget rather than uploaded in one stall.
        // Next texture to upload. Uploading them all at once cost 40ms on a
        // transport - the images are large, and the expense is the GPU upload
        // rather than the decode, which the worker already did.
        size_t nextTextureIndex = 0;
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
    std::optional<ModelLoadResult> admitModel(uint32_t id, bool hasTexturePaths);
    /// Preparations loadModelIncremental made itself, for models whose loads
    /// are still in progress.
    std::unordered_map<uint32_t, std::shared_ptr<WmoModelPrep>> preparedHere_;

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
    /// From the shared cache, loading what is not there.
    LoadedTexture loadTexture(const std::string& path);
    /// The normal/height map of a loaded texture, from the cache, the terrain
    /// workers' prepared pixels, or - outside streaming - made here.
    TextureRef loadNormalHeightMap(const std::string& resolvedPath, const pipeline::BLPImage* decoded);
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;
    std::unordered_map<std::string, EncodedNormalMap>* predecodedNormalMapCache_ = nullptr;

    /**
     * Allocate a material descriptor set from the pool
     */
    VkDescriptorSet allocateMaterialSet();
    /// Create a merged batch's material buffer and descriptor set.
    void createMergedBatchMaterial(const ModelData& model, const GroupResources& group,
                                   GroupResources::MergedBatch& mb);
    /// Point a batch at the normal/height map made from its texture, if any.
    void attachNormalHeightMap(const ModelData& model, uint32_t texIndex,
                               GroupResources::MergedBatch& mb) const;
    /// Ask again for textures that failed to load and give the batches drawn
    /// white for want of them their texture. A failed MPQ read is often
    /// transient.
    void retryPendingTextures();
    std::vector<uint32_t> modelsWithPendingTextures_;

    /**
     * Destroy GPU resources for a single group.
     * When defer=true, destruction is scheduled via deferAfterAllFrameFences
     * so in-flight command buffers are not invalidated.
     */
    void destroyGroupGPU(GroupResources& group, bool defer = false);


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
    void registerRtModel(uint32_t modelId, const ModelData& model, WmoModelPrep& prep);
    void releaseRtModel(uint32_t modelId);

    collision::CollisionWorld* collision_ = nullptr;

    // Vulkan pipelines
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;
    VkPipeline wireframePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering (Phase 7)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;
    /// Sets binding an alpha-keyed material's texture for the shadow pass, so
    /// a lattice casts the lattice and not a solid pane. One pool per frame in
    /// flight, reset at the top of that frame's shadow pass.
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    VkDescriptorSet shadowTexSet(VkTexture* tex, uint32_t frameIndex);

    // Descriptor set layouts
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;

    // Descriptor pool for material sets
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    /// Every merged batch's material parameters (uniform_slot_pool.hpp).
    std::shared_ptr<UniformSlotPool> materialSlots_;
    static constexpr uint32_t MAX_MATERIAL_SETS = 32768;

    /// When a path that would not load, or found no room, is worth asking for
    /// again: a failed MPQ read is often transient.
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint64_t textureLookupSerial_ = 0;

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
    bool deferNormalMaps_ = false;       // skip normal map gen during streaming
    float normalMapStrength_ = 0.8f;     // 0.0 = flat, 1.0 = full, 2.0 = exaggerated
    bool pomEnabled_ = true;             // on by default
    int pomQuality_ = 1;                 // 0=Low(16), 1=Medium(32), 2=High(64)

    // Rendering state
    bool wireframeMode = false;
    bool frustumCulling = true;  // conservative world-group bounds, independently per view
    bool portalCulling = true;   // AABB transform bug fixed; conservative frustum test (no plane-side check) is visually safe
    bool distanceCulling = false;  // Disabled - causes ground to disappear

    /// Optional portal and tighter group culling stay off by default:
    /// they historically removed nearby building interiors. The common horizontal
    /// view-distance limit always applies, matching terrain even with this off.
    /// WOWEE_WMO_CULL=1 enables the additional culling for diagnostics.
    bool cullingEnabled_ = false;
    float maxGroupDistance = 500.0f;
    float viewDistance_ = 1200.0f;
    uint32_t shadowDrawCalls_ = 0;

    /// See contentRevision(). Bumped by markContentChanged().
    uint64_t contentRevision_ = 1;
    void markContentChanged() { ++contentRevision_; }

    // Parallel visibility culling
    uint32_t numCullThreads_ = 1;

    /// One instance as a pass draws it: what the recording needs, copied out
    /// of the scene by the cull, so the recording reads no instance.
    struct GroupDraw {
        std::shared_ptr<const GroupDrawData> gpu;
        glm::vec3 worldMin{0}, worldMax{0};
        LocalLightSet lights = kNoLocalLights;
    };
    struct DrawEntry {
        glm::mat4 modelMatrix{1.0f};
        float scale = 1.0f;
        /// Valid for the frame: models are unloaded only between frames.
        /// The model's groups this pass draws.
        std::vector<GroupDraw> groups;
    };

    /// What one pass draws, rebuilt by its cull each time it runs. Entries
    /// past `used` are last frame's, kept for their capacity and never read.
    struct DrawList {
        std::vector<DrawEntry> entries;
        size_t used = 0;
        glm::vec3 cameraPosition{0};
        uint32_t frustumCulled = 0;
        uint32_t portalCulled = 0;
        uint32_t distanceCulled = 0;
        uint32_t drawCalls = 0;
        /// The portal walk's result for the instance being culled.
        std::unordered_set<uint32_t> portalGroups;

        void clear() {
            used = 0;
            frustumCulled = portalCulled = distanceCulled = drawCalls = 0;
        }
        DrawEntry& add() {
            if (used == entries.size()) entries.emplace_back();
            DrawEntry& entry = entries[used++];
            entry.groups.clear();
            return entry;
        }
        /// Takes back the entry add() gave, when it ended up drawing nothing.
        void dropLast() { --used; }
        [[nodiscard]] std::span<const DrawEntry> drawn() const { return {entries.data(), used}; }
    };
    uint32_t viewDrawCallCount_ = 0;
    /// The depth-only passes', one at a time on the main thread.


    /// The instances the camera sees and their groups that are drawn.
    void cullView(const SceneView& camera, const glm::vec3& portalViewerPos, DrawList& list) const;
    uint32_t recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const glm::vec3& camPos,
                    const DrawList& list) const;
    /// The instances, shown, and groups that reach a shadow map of this centre and reach.
    void extractDepth(DrawList& list) const;
    GroupDraw extractGroup(const GroupResources& group, const WMOInstance& instance, uint32_t groupIndex) const;

    struct InstanceState {
        std::unordered_map<uint32_t, ModelData> loadingModels_;
        std::vector<WMOInstance> instances;
        RtInstanceChanges rtChanges;
        std::unordered_map<uint32_t, size_t> instanceIndexById;
    };
    InstanceState defaultInstances_;
    InstanceState* scene_ = &defaultInstances_;

public:
    struct Frame {
        void clear() noexcept;
        DrawList view, reflection, depth;
        bool wireframe = false;
        int normalMap = 0, pom = 0, samples = 0;
        float normalStrength = 1;
    };
private:
    FramePool<Frame> framePool_;
    std::shared_ptr<const Frame> recording_ = std::make_shared<Frame>();

};

} // namespace rendering
} // namespace wowee
