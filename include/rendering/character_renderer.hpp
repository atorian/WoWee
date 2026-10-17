#pragma once

#include "rendering/normal_map_builder.hpp"
#include "rendering/texture_cache.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/local_lights.hpp"

#include "pipeline/m2_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <future>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <functional>
#include <optional>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

// Forward declarations
class Camera;
class VkContext;
class VkTexture;

// Enchant visual (glint, glow) attached to a weapon at one of the weapon model's
// own item-visual attachment points.
struct WeaponEffectAttachment {
    uint32_t effectModelId;
    uint32_t effectInstanceId;
    glm::vec3 offset;          // attachment position on the weapon model
};

// Weapon attached to a character instance at a bone attachment point
struct WeaponAttachment {
    uint32_t weaponModelId;
    uint32_t weaponInstanceId;
    uint32_t attachmentId;     // 1=RightHand, 2=LeftHand
    uint16_t boneIndex;
    glm::vec3 offset;
    glm::mat4 localTransform{1.0f}; // sheath/hand orientation after attachment point
    float sheathPush = 0.0f;   // smoothed outward push when the arm swings into the blade
    std::vector<WeaponEffectAttachment> effects;
};

/**
 * Character renderer for M2 models with skeletal animation
 *
 * Features:
 * - Skeletal animation with bone transformations
 * - Keyframe interpolation (linear position/scale, slerp rotation)
 * - Vertex skinning (GPU-accelerated via bone SSBO)
 * - Texture loading from BLP via AssetManager
 */
class CharacterRenderer {
    struct InstanceState;
public:
    /// The owning scene holds this lease. Geometry/texture caches stay here;
    /// instances, collision indexes and incomplete uploads die with the lease.
    class InstanceSession {
    public:
        explicit InstanceSession(CharacterRenderer& renderer);
        ~InstanceSession();
        InstanceSession(const InstanceSession&) = delete;
        InstanceSession& operator=(const InstanceSession&) = delete;
    private:
        CharacterRenderer& renderer_;
        std::unique_ptr<InstanceState> state_;
    };
    CharacterRenderer();
    ~CharacterRenderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, pipeline::AssetManager* am,
                    VkRenderPass renderPassOverride = VK_NULL_HANDLE,
                    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT);
    void shutdown();
    void clear();  // Remove all models/instances/textures but keep pipelines/pools
    void clearInstances();  // Retain process model/material caches.

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }

    bool loadModel(const pipeline::M2Model& model, uint32_t id);

    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                           const glm::vec3& rotation = glm::vec3(0.0f),
                           float scale = 1.0f);

    // oneShotReturnAnim: animation to resume (looping) when a non-looping
    // animation finishes; 0 = Stand. Lets NPC one-shot emotes return to their
    // persistent work/state loop instead of idling.
    void playAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true,
                       uint32_t oneShotReturnAnim = 0);
    /// Play other animations on each arm alone, until the next playAnimation.
    void setArmAnimations(uint32_t instanceId, uint32_t leftArmAnim, uint32_t rightArmAnim);

    /// Advance every instance's animation and pose its skeleton. `view` is
    /// what billboard bones turn against; without one they stay as authored.
    void update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4* view);

    /** Pre-allocate GPU resources (bone SSBOs, descriptors) on main thread before parallel render. */
    void prepareRender(uint32_t frameIndex);
    /// Which instances a render call draws. A faded one - an aura's
    /// translucent body, a ghost, a fade-in - writes depth, so whatever is
    /// drawn after it inside or behind it is hidden: Shadowform's fire,
    /// which rises from the feet through the body, was gone. Faded ones are
    /// drawn after the blended world instead, laid over it as the client does.
    enum class Part { All, Solid, Faded };
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                Part part = Part::All);
    /// The pass faded instances are drawn in when it is not the scene's - the
    /// one after the water that blended M2s and particles go to - or null.
    /// Rebuilds pipelines: call between frames, with nothing in flight.
    void setFadedPass(VkRenderPass pass, VkSampleCountFlagBits samples);
    [[nodiscard]] VkRenderPass fadedPass() const { return fadedPass_; }
    void recreatePipelines();
    /// The main-pass pipelines, which initialize() and recreatePipelines()
    /// both need and each used to describe.
    void buildMainPassPipelines(VkDevice device, VkRenderPass mainPass,
                                VkSampleCountFlagBits samples,
                                wowee::rendering::VkShaderModule& charVert,
                                wowee::rendering::VkShaderModule& charFrag);
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f,
                      bool resetDescriptors = true);

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    void setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation);
    /// How fast the instance's animation runs against real time: 1 normally.
    /// Another player's movement played back faster to catch up runs its
    /// animation faster with it.
    void setInstanceAnimationSpeed(uint32_t instanceId, float speed);
    void setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad);
    /// Look up again, for each instance that has moved since it was last
    /// looked up or whose buildings have changed since, the light of the WMO
    /// interior group it stands in. `interiorLightAt` answers for a world
    /// position, no value being outdoors; `worldRevision` changes whenever the
    /// buildings it answers from do (WMORenderer::contentRevision).
    void updateInteriorLight(
        const std::function<std::optional<InteriorLight>(const glm::vec3&)>& interiorLightAt,
        uint64_t worldRevision);
    void startFadeIn(uint32_t instanceId, float durationSeconds);
    void setInstanceOpacity(uint32_t instanceId, float opacity);
    /// How far the camera's closeness fades the instance - the player, with
    /// the camera pressed against them - multiplied on top of everything
    /// else that makes it see-through. Carried to its weapons and their
    /// enchants and to its look companions (a rider's mount).
    void setInstanceCameraFade(uint32_t instanceId, float alpha);
    /// The look an aura gives a unit, from its SpellVisualKit's character
    /// procs: the lit colour is multiplied by `tint`, and the unit is drawn
    /// no more opaque than `alpha`. White and 1 take it off. Carried to the
    /// instance's weapons and their enchants, and to its look companions.
    void setInstanceStateLook(uint32_t instanceId, const glm::vec3& tint, float alpha);
    /// Draws `companionId` in `instanceId`'s aura look from now on, as it is
    /// and as it changes: a mount takes its rider's, so a Shadowform priest's
    /// mount goes dark and translucent with them. Ends with either instance.
    void addLookCompanion(uint32_t instanceId, uint32_t companionId);
    [[nodiscard]] const pipeline::M2Model* getModelData(uint32_t modelId) const;
    [[nodiscard]] const pipeline::M2Model* getInstanceModelData(uint32_t instanceId) const;
    void setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets);
    /// Opt an instance into the Skin Extra head-detail batch. Only a character
    /// whose type 8 slot has been filled from CharSections should ask for it.
    void setDrawSkinExtra(uint32_t instanceId, bool enabled);

    /// Counts the head-batch diagnostic lines so it stops after a few.
    int headBatchCanaryCount_ = 0;
    /// The instance holds `texture` while it draws with it; an empty one
    /// clears the group's override.
    void setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, TextureRef texture);
    void setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, TextureRef texture);
    void clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot);
    void setInstanceVisible(uint32_t instanceId, bool visible);
    void removeInstance(uint32_t instanceId);
    bool getAnimationState(uint32_t instanceId, uint32_t& animationId, float& animationTimeMs, float& animationDurationMs) const;
    /// Footfall ($FSD) event times in ms for the sequence the instance is
    /// currently playing; nullptr if the model has none for that sequence.
    [[nodiscard]] const std::vector<uint32_t>* getFootstepEventTimes(uint32_t instanceId) const;
    [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
    bool getAnimationSequences(uint32_t instanceId, std::vector<pipeline::M2Sequence>& out) const;
    bool getInstanceModelName(uint32_t instanceId, std::string& modelName) const;
    bool getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const;

    /// How tall this character stands, from its feet to the top of its head,
    /// with its scale applied. The model origin is at the feet.
    bool getInstanceHeight(uint32_t instanceId, float& outHeight) const;

    /// Where one of the skeleton's named bones sits, in model Z with the
    /// instance's scale applied. The key bone ids are WoW's own: 4 is the
    /// lower spine, 6 the head, 7 the jaw.
    ///
    /// A bone is where a thing actually is. A fraction of the bounding box is
    /// where it usually is, which is not the same for a race whose hair is
    /// half the distance from its chin to the top of the box.
    bool getInstanceKeyBonePivotZ(uint32_t instanceId, int32_t keyBoneId, float& outZ) const;
    bool getInstanceFootZ(uint32_t instanceId, float& outFootZ) const;
    bool getInstancePosition(uint32_t instanceId, glm::vec3& outPos) const;
    /// Where the instance stands, which way it faces and how large it is: the
    /// matrix its model is drawn with, the feet at its origin.
    bool getInstanceModelMatrix(uint32_t instanceId, glm::mat4& outMatrix) const;
    /// How opaque the instance is drawn: the lower of its fade and its aura
    /// look's alpha. What is hung on it fades with it.
    bool getInstanceDrawnOpacity(uint32_t instanceId, float& outOpacity) const;

    /** Debug: Log all available animations for an instance */
    void dumpAnimations(uint32_t instanceId) const;

    /** Attach a weapon model to a character instance at the given attachment point. */
    bool attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                      const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                      const std::string& texturePath,
                      const glm::mat4& localTransform = glm::mat4(1.0f));

    /** Detach a weapon from the given attachment point (drops its enchant effects too). */
    void detachWeapon(uint32_t charInstanceId, uint32_t attachmentId);
    /** Move the weapon at one attachment point to another, the same model and
     *  instance with its enchants: sheathing and drawing need no reload. False,
     *  and logged, when the character's model has no such point. */
    bool moveWeapon(uint32_t charInstanceId, uint32_t fromAttachmentId,
                    uint32_t toAttachmentId, const glm::mat4& localTransform);

    // Free a model's GPU buffers and map entry once no instance references it.
    // For per-instance model ids (weapons/effects/player composites, which get
    // a fresh id per attach or spawn) - without this every reload or despawn
    // leaked the model. Do NOT call for displayId-keyed NPC models; those are
    // cached across despawn/respawn on purpose. Buffers are destroyed via the
    // frame-fence deferral path; shared textures stay in the cache.
    void unloadModelIfUnused(uint32_t modelId);

    /// An instance whose model has particle or ribbon emitters, as the last
    /// update() left it. This renderer draws meshes only; CharacterEmitters
    /// runs the emitters on this skeleton. `bones` is valid until the next
    /// update() and may be empty for a model that is not animated (a weapon).
    struct EmitterSource {
        uint32_t instanceId = 0;
        uint64_t modelSerial = 0;
        const pipeline::M2Model* model = nullptr;
        glm::mat4 modelMatrix{1.0f};
        float scale = 1.0f;
        const std::vector<glm::mat4>* bones = nullptr;
        int sequenceIndex = 0;
        float animationTimeMs = 0.0f;
        float globalSequenceTimeMs = 0.0f;
        /// Moved by the last update() and shown.
        bool posed = false;
    };
    void forEachEmitterSource(const std::function<void(const EmitterSource&)>& visit) const;

    // Model id an instance renders with (0 if the instance doesn't exist).
    [[nodiscard]] uint32_t getInstanceModelId(uint32_t instanceId) const {
        auto it = scene_->instances.find(instanceId);
        return it != scene_->instances.end() ? it->second.modelId : 0;
    }

    /**
     * Attach an enchant visual to the weapon at the given attachment point.
     * visualSlot is the ItemVisuals.dbc slot (0-4), which selects the attachment
     * point on the weapon model the effect hangs from.
     */
    bool attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId, uint32_t visualSlot,
                            const pipeline::M2Model& effectModel, uint32_t effectModelId);

    /** Remove all enchant visuals from the weapon at the given attachment point. */
    void detachWeaponEffects(uint32_t charInstanceId, uint32_t attachmentId);

    /** Mark an instance as a scene backdrop: no culling, no character material heuristics. */
    void setInstanceSceneModel(uint32_t instanceId, bool isScene);


    /** Get the world-space transform of an attachment point on an instance. */
    bool getAttachmentTransform(uint32_t instanceId, uint32_t attachmentId, glm::mat4& outTransform);

    [[nodiscard]] size_t getInstanceCount() const { return scene_->instances.size(); }
    [[nodiscard]] size_t getModelCount() const { return models.size(); }
    /// Normal maps queued, being derived, or derived and not yet uploaded.
    [[nodiscard]] size_t pendingNormalMapJobs() const;

    // Normal mapping settings. There are no parallax settings: characters are
    // drawn without parallax whatever the world uses (see the material setup).
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; }
    void setNormalMapStrength(float strength) { normalMapStrength_ = strength; }

    // Fog/lighting/shadow are now in per-frame UBO - keep stubs for callers that haven't been updated
    void setFog(const glm::vec3&, float, float) {}
    void setLighting(const float[3], const float[3], const float[3]) {}
    void setShadowMap(VkTexture*, const glm::mat4&) {}
    void clearShadowMap() {}

    // Pre-decoded BLP cache: set before calling loadModel() to skip main-thread BLP decode
    static pipeline::BLPImage prepareTexture(pipeline::AssetManager* assets, const std::string& path);
    // CPU-only preparation: safe on workers; no renderer/GPU state is accessed.
    static void prepareCompositeTextures(pipeline::AssetManager* assets,
        const std::vector<std::string>& layers,
        std::unordered_map<std::string, pipeline::BLPImage>& decoded);
    static void prepareCompositeWithRegions(pipeline::AssetManager* assets,
        const std::string& base, const std::vector<std::string>& layers,
        const std::vector<std::pair<int, std::string>>& regions,
        std::unordered_map<std::string, pipeline::BLPImage>& decoded);
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache,
                              bool preparedOnly = false) {
        predecodedBLPCache_ = cache;
        preparedTexturesOnly_ = cache && preparedOnly;
    }

    /// Append the point lights of the units animated this frame - those in
    /// view, and those within 33 yards (local_lights.hpp).
    void gatherLocalLights(const Camera& camera, std::vector<LocalLight>& out) const;
    /// The frame's point lights, which every unit picks its nearest from.
    void setFrameLocalLights(const std::vector<LocalLight>* lights) { frameLights_ = lights; }

private:
    const std::vector<LocalLight>* frameLights_ = nullptr;
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;
    bool preparedTexturesOnly_ = false;
    // GPU representation of M2 model
    struct M2ModelGPU {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;

        pipeline::M2Model data;  // Original model data
        std::vector<glm::mat4> bindPose;  // Inverse bind pose matrices

        // Tight bind-pose bounds from rendered vertices. M2 header
        // boundingBox/boundingRadius describe collision geometry on many
        // creatures and can cover little more than their feet.
        glm::vec3 visualBoundMin{0.0f};
        glm::vec3 visualBoundMax{0.0f};
        float visualBoundRadius = 0.0f;

        // Textures loaded from BLP (indexed by texture array position), held
        // for as long as the model is.
        std::vector<TextureRef> textureIds;

        // Cached batch render order sorted by (priorityPlane, materialLayer).
        // Built once at load time - the sort only depends on the model's static
        // batch metadata, so doing it per-instance per-frame in render() was
        // pure overhead.
        std::vector<size_t> sortedBatchIndices;

        // Pre-classified at load time to avoid per-batch string ops in render loop
        bool isSkyBird = false;

        /// Particle or ribbon emitters, which CharacterEmitters runs.
        bool hasEmitters = false;
        /// Which load this is. A model id can be loaded again with other
        /// data, so the id alone does not say what was loaded.
        uint64_t loadSerial = 0;
    };

    // Character instance
    struct CharacterInstance {
        uint32_t id;
        uint32_t modelId;

        glm::vec3 position;
        glm::vec3 rotation;
        float scale;
        bool visible = true;  // For first-person camera hiding
        float torsoYawOverrideRad = 0.0f;

        // Animation state
        uint32_t currentAnimationId = 0;
        int currentSequenceIndex = -1;  // Index into M2Model::sequences
        int armSequenceIndex[2] = {-1, -1};  // Left, right arm on their own sequences; -1 follows the body
        bool armSequenceLoops[2] = {false, false};  // Idle arm loops; a reaching arm holds its last frame
        std::vector<int8_t> boneArm;  // Per bone: 0 left arm, 1 right arm, -1 neither; built by setArmAnimations
        float animationTime = 0.0f;
        float animationSpeed = 1.0f;     // Sequence time per real time; global sequences keep real time
        float globalSequenceTime = 0.0f; // Separate timer for global sequences (accumulates without wrapping at sequence duration)
        bool animationLoop = true;
        uint32_t oneShotReturnAnim = 0; // Anim to resume when a one-shot ends (0 = Stand)
        bool isDead = false;  // Prevents movement while in death state
        std::vector<glm::mat4> boneMatrices;  // Current bone transforms

        // Geoset visibility - which submesh IDs to render
        // Empty = render all (for non-character models)
        std::unordered_set<uint16_t> activeGeosets;
        /// Draw the Skin Extra (texture type 8) head-detail batch. True only
        /// where the instance has been set up to composite it - the player.
        bool drawSkinExtra = false;

        // Per-geoset-group texture overrides (group → texture), held by the
        // instance.
        std::unordered_map<uint16_t, TextureRef> groupTextureOverrides;

        // Per-texture-slot overrides (slot → texture), held by the instance.
        std::unordered_map<uint16_t, TextureRef> textureSlotOverrides;

        // Weapon attachments (weapons parented to this instance's bones)
        std::vector<WeaponAttachment> weaponAttachments;

        // Opacity (for fade-in)
        float opacity = 1.0f;
        float fadeInTime = 0.0f;     // elapsed fade time (seconds)
        float fadeInDuration = 0.0f; // total fade duration (0 = no fade)
        // An aura's look (setInstanceStateLook). Apart from `opacity`, which
        // fades, stealth and ghosts write; the two meet at draw time.
        glm::vec3 stateTint{1.0f};
        float stateAlpha = 1.0f;
        /// setInstanceCameraFade: multiplied on top of the two above.
        float cameraFade = 1.0f;
        // Instances drawn in this one's aura look (addLookCompanion). Ids are
        // never reused, so one removed since is simply not found.
        std::vector<uint32_t> lookCompanions;
        // The body an attachment - a weapon, a helm, an enchant's effect -
        // is part of; 0 for a body of its own. An attachment keeps no
        // visibility, opacity or look of its own: it is shown, hidden,
        // faded and tinted as its body is (drawnAs). Copied when it was
        // attached, equipment that arrived after a druid's body was put away
        // for a form floated over the bear.
        uint32_t bodyId = 0;

        // Override model matrix (used for weapon instances positioned by parent bone)
        bool hasOverrideModelMatrix = false;
        glm::mat4 overrideModelMatrix{1.0f};

        /// The light of the WMO interior group the instance stands in, if any.
        InteriorLightGPU interiorLight;
        /// Where interiorLight was looked up, and against which buildings.
        InteriorLightProbe interiorLightProbe;

        // A scene rather than a character: the glue-screen backdrops. Two things
        // follow. Their origin can sit hundreds of units from their geometry, so
        // culling on it would drop them. And the material heuristics below exist to
        // rescue character textures - applied to a scene they erase it, because
        // Stormwind's walls are DXT5 with an unused alpha channel that the opaque
        // batches must ignore, exactly as the blend mode says.
        bool isSceneModel = false;


        // Bone update throttling for characters outside normal gameplay range.
        uint32_t boneUpdateCounter = 0;
        /// The last update() moved this instance - its clock, its skeleton or,
        /// for a weapon, its place on its wielder. Not when it was out of
        /// animation range and left as it was.
        bool posedThisFrame = false;
        const M2ModelGPU* cachedModel = nullptr;  // Avoid per-frame hash lookups

        // Per-instance bone SSBO (double-buffered per frame)
        VkBuffer boneBuffer[2] = {};
        VmaAllocation boneAlloc[2] = {};
        void* boneMapped[2] = {};
        VkDescriptorSet boneSet[2] = {};
    };

    void setupModelBuffers(M2ModelGPU& gpuModel);
    void calculateBindPose(M2ModelGPU& gpuModel);
    void calculateBoneMatrices(CharacterInstance& instance);
    /// One instance's clock: its sequence and its global sequences.
    void advanceAnimation(uint32_t instanceId, CharacterInstance& instance, float deltaTime);
    /// calculateBoneMatrices for each, spread over the frame workers.
    void poseInstances(std::vector<std::reference_wrapper<CharacterInstance>>& instances);
    glm::mat4 getBoneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
                               int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations);
    [[nodiscard]] glm::mat4 getModelMatrix(const CharacterInstance& instance) const;
    /// Whose opacity and aura look `instance` is drawn with: its body's for
    /// an attachment, its own otherwise.
    [[nodiscard]] const CharacterInstance& drawnAs(const CharacterInstance& instance) const;
    /// Drawn at all: neither it nor, for an attachment, its body is hidden.
    [[nodiscard]] bool isShown(const CharacterInstance& instance) const;
    void destroyModelGPU(M2ModelGPU& gpuModel, bool defer = false);
    void destroyInstanceBones(CharacterInstance& inst, bool defer = false);

    // Attachment point lookup helper - shared by attachWeapon() and getAttachmentTransform()
    bool findAttachmentBone(uint32_t modelId, uint32_t attachmentId,
                           uint16_t& outBoneIndex, glm::vec3& outOffset) const;

public:
    /**
     * Build a composited character skin texture by alpha-blending overlay
     * layers onto a base skin BLP. A composite already made from the same
     * layers is shared. White when it cannot be made.
     */
    TextureRef compositeTextures(const std::vector<std::string>& layerPaths);

    /**
     * Build a composited character skin with explicit region-based equipment
     * overlays; shared like compositeTextures.
     */
    TextureRef compositeWithRegions(const std::string& basePath,
                                    const std::vector<std::string>& baseLayers,
                                    const std::vector<std::pair<int, std::string>>& regionLayers);

    /** A BLP texture from the shared cache, decoded the way characters key it; white on failure. */
    TextureRef loadTexture(const std::string& path);
    [[nodiscard]] const TextureRef& whiteTexture() const { return whiteTexture_; }

    /** Replace a loaded model's texture at the given slot; the model holds it. */
    void setModelTexture(uint32_t modelId, uint32_t textureSlot, TextureRef texture);



private:
    // Create 1×1 fallback textures used when real textures are missing or still loading.
    void createFallbackTextures();

    VkContext* vkCtx_ = nullptr;
    VkRenderPass renderPassOverride_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamplesOverride_ = VK_SAMPLE_COUNT_1_BIT;
    pipeline::AssetManager* assetManager = nullptr;

    // Vulkan pipelines, one per blend mode, for one render pass.
    struct PipelineSet {
        VkPipeline opaque = VK_NULL_HANDLE;
        VkPipeline alphaTest = VK_NULL_HANDLE;
        VkPipeline alpha = VK_NULL_HANDLE;
        VkPipeline additive = VK_NULL_HANDLE;
        VkPipeline modulate = VK_NULL_HANDLE;      // blend mode 5
        VkPipeline modulate2x = VK_NULL_HANDLE;    // blend mode 6
        // Whole-instance fades (ghost form, spawn fade-in): alpha blend with
        // depth write kept on, so the faded model still self-occludes instead
        // of showing backfaces and under-armor skin through the body.
        VkPipeline translucent = VK_NULL_HANDLE;
        // A faded body (Shadowform, a ghost, a fade-in) in two steps: its
        // solid parts into depth alone, then colour only where that depth is,
        // so it is one even layer - the near legs hiding the far ones - not a
        // stack of every surface in the model's own drawing order.
        VkPipeline fadeDepth = VK_NULL_HANDLE;
        VkPipeline fadeColor = VK_NULL_HANDLE;
        [[nodiscard]] bool complete() const {
            return opaque && alphaTest && alpha && additive && modulate && modulate2x &&
                   translucent && fadeDepth && fadeColor;
        }
    };
    PipelineSet buildPipelineSet(VkDevice device, VkRenderPass pass, VkSampleCountFlagBits samples,
                                 wowee::rendering::VkShaderModule& charVert,
                                 wowee::rendering::VkShaderModule& charFrag);
    static void destroyPipelineSet(VkDevice device, PipelineSet& set);
    PipelineSet scenePipes_;          // the scene pass
    PipelineSet fadedPassPipes_;      // fadedPass_, when there is one
    VkRenderPass fadedPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits fadedPassSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;  // set 0 (owned by Renderer)
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;  // set 2

    // Descriptor pool
    VkDescriptorPool materialDescPools_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    struct MaterialDescriptorKey {
        VkImageView diffuse = VK_NULL_HANDLE;
        VkImageView normal = VK_NULL_HANDLE;
        VkImageView second = VK_NULL_HANDLE;
        VkSampler diffuseSampler = VK_NULL_HANDLE;
        VkSampler normalSampler = VK_NULL_HANDLE;
        VkSampler secondSampler = VK_NULL_HANDLE;
        bool operator==(const MaterialDescriptorKey&) const = default;
    };
    struct MaterialDescriptorKeyHash {
        size_t operator()(const MaterialDescriptorKey& key) const {
            const size_t a = std::hash<VkImageView>{}(key.diffuse);
            const size_t b = std::hash<VkImageView>{}(key.normal);
            const size_t c = std::hash<VkSampler>{}(key.diffuseSampler);
            const size_t d = std::hash<VkSampler>{}(key.normalSampler);
            const size_t e = std::hash<VkImageView>{}(key.second);
            const size_t f = std::hash<VkSampler>{}(key.secondSampler);
            return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ (e << 4) ^ (f << 5);
        }
    };
    std::unordered_map<MaterialDescriptorKey, VkDescriptorSet, MaterialDescriptorKeyHash>
        materialDescriptorCache_[2];
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    std::shared_ptr<std::atomic<uint64_t>> boneDescPoolGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    uint32_t lastMaterialPoolResetFrame_ = 0xFFFFFFFFu;

    // Material UBO ring buffer - pre-allocated per frame slot, sub-allocated each draw
    VkBuffer materialRingBuffer_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation materialRingAlloc_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* materialRingMapped_[2] = {nullptr, nullptr};
    uint32_t materialRingOffset_[2] = {0, 0};
    uint32_t materialUboAlignment_ = 256;  // minUniformBufferOffsetAlignment
    static constexpr uint32_t MATERIAL_RING_CAPACITY = 4096;

    /// When a path that would not load, or found no room, is next worth a read.
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;  // dedup warning logs
    uint64_t textureLookupSerial_ = 0;
    // Fallbacks, held in the shared cache like any other texture.
    TextureRef whiteTexture_;
    TextureRef flatNormalTexture_;

    std::unordered_map<uint32_t, M2ModelGPU> models;

    uint32_t nextInstanceId = 1;
    uint64_t nextModelLoadSerial_ = 1;

    /// Derives the normal/height maps of the textures characters are drawn
    /// with, on two workers of its own from a bounded queue, nearest first.
    /// Asked by processPendingNormalMaps from what the instances draw.
    std::unique_ptr<NormalMapBuilder> normalMapBuilder_;
    /// Keys whose map could not be built; not asked for again.
    std::unordered_set<std::string> normalMapsFailed_;
    /// Where the camera was at the last update: what "nearest" is measured from.
    glm::vec3 lastCameraPos_{0.0f};
    /// Frames since the instances were last walked for maps to ask for.
    uint32_t framesSinceNormalMapScan_ = 0;
    /// Ask the builder for the maps the drawn instances lack, nearest first.
    void requestNormalMaps();
public:
    /// Upload up to `budget` finished normal maps and attach each to its
    /// texture, and every few frames ask for the ones still missing. Main
    /// thread, once a frame.
    void processPendingNormalMaps(int budget = 4);
private:

    // Normal mapping settings
    bool normalMappingEnabled_ = true;
    float normalMapStrength_ = 0.8f;

    // Maximum bones supported
    static constexpr int MAX_BONES = 240;
    uint32_t numAnimThreads_ = 1;
    std::vector<std::future<void>> animFutures_;
    std::vector<std::reference_wrapper<CharacterInstance>> toUpdate_;  // reused across frames
    /// The view update() was given, for the billboard bones it poses.
    std::optional<glm::mat4> boneView_;

    // Shadow pipeline resources
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    /// A batch's texture and the M2Texture wrap flags of the slot it came from
    /// (bit 0 repeats U, bit 1 repeats V). The wrap belongs to the slot, not
    /// to the image, which another model may tile where this one clamps.
    struct BatchTexture {
        static constexpr uint8_t kRepeatBoth = 0x3;
        /// The model's or the instance's reference; never null.
        const TextureRef* texture = nullptr;
        uint8_t wrapFlags = kRepeatBoth;
    };

    /// The texture of one of a batch's stages, its slot override applied.
    /// Shared by the main pass and the shadow pass, which needs stage 0 to
    /// cut a silhouette rather than a rectangle.
    [[nodiscard]] BatchTexture resolveBatchTexture(const CharacterInstance& inst,
                                                   const M2ModelGPU& gm,
                                                   const pipeline::M2Batch& b,
                                                   uint32_t stage = 0) const;

    /// Per-batch texture sets for alpha-keyed shadow casters, one pool per
    /// frame in flight and reset at the top of each frame's shadow pass. Same
    /// arrangement M2Renderer uses for its foliage shadows.
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    VkDescriptorSet shadowTexDescSet(VkTexture* tex, uint32_t frameIndex);
    struct InstanceState {
        std::unordered_map<uint32_t, CharacterInstance> instances;
    };
    InstanceState defaultInstances_;
    InstanceState* scene_ = &defaultInstances_;

};

} // namespace rendering
} // namespace wowee
