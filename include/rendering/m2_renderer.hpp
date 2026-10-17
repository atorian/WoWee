#pragma once

#include "rendering/texture_cache.hpp"
#include "rendering/collision_geometry.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/spatial_grid.hpp"
#include "rendering/m2_cull_grid.hpp"
#include "rendering/shadow_params.hpp"

#include "pipeline/m2_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/grass_clearing.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/m2_blend_mode.hpp"
#include "rendering/m2_uv_transform.hpp"
#include "rendering/frames_in_flight.hpp"
#include "rendering/game_object_animation.hpp"
#include "rendering/m2_combiner.hpp"
#include "rendering/local_lights.hpp"
#include "rendering/wind_field.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <string>
#include <functional>
#include <optional>
#include <random>
#include <chrono>
#include <future>
#include <algorithm>

namespace wowee {

namespace pipeline {
    class AssetManager;
}

namespace rendering {

class Camera;
class Frustum;
class VkContext;
class VkTexture;
class HiZSystem;
class RtScene;

// Ceiling on the bone matrices computed and uploaded for one M2 instance.
// WoW models run well past a hundred bones - the largest shipped model has
// 315 - and a vertex weighted to a bone above this ceiling would read outside
// its instance's range in the mega bone buffer, which is what threw stray
// spikes out of water elementals and other high-bone-count creatures.
inline constexpr uint32_t kMaxBonesPerInstance = 512;

/**
 * GPU representation of an M2 model
 */
struct M2ModelGPU {
    struct BatchGPU {
        VkTexture* texture = nullptr;  // from cache, NOT owned
        VkTexture* secondTexture = nullptr; // the second stage, where the combiner reads one
        uint8_t secondTexFlags = 0;         // M2Texture.flags of the second stage's slot
        uint16_t colorTrackIndex = UINT16_MAX;
        uint16_t weightTrackIndex = UINT16_MAX;
        /// How the stages combine (m2BatchShading), and where the second
        /// stage's coordinates come from, as m2.vert's texCoordSet2 reads it.
        M2Combiner combiner = M2Combiner::Mod;
        int8_t secondTexCoords = 1;
        /// False for shader 0x8000, which the client does not draw.
        bool shaderDrawn = true;
        VkDescriptorSet materialSet = VK_NULL_HANDLE;  // set 1
        ::VkBuffer materialUBO = VK_NULL_HANDLE;
        VmaAllocation materialUBOAlloc = VK_NULL_HANDLE;
        void* materialUBOMapped = nullptr;  // cached mapped pointer (avoids per-frame vmaGetAllocationInfo)
        uint32_t indexStart = 0;   // offset in indices (not bytes)
        uint32_t indexCount = 0;
        bool hasAlpha = false;
        /// The alpha is a real silhouette, not an atlas leftover.
        bool alphaIsSilhouette = false;
        glm::vec3 tint{1.0f};  ///< the batch's authored colour
        uint16_t textureAnimIndex = 0xFFFF; // 0xFFFF = no texture animation
        uint16_t blendMode = 0;   // 0=Opaque, 1=AlphaKey, 2=Alpha, 3=Add, etc.
        M2BatchDrawState drawState; // decided by writeBatchMaterial
        int8_t priorityPlane = 0; // authored transparent layer priority
        uint16_t materialFlags = 0; // M2 material flags (0x01=Unlit, 0x04=TwoSided, 0x10=NoDepthWrite)
        uint16_t submeshLevel = 0; // LOD level: 0=base, 1=LOD1, 2=LOD2, 3=LOD3
        int8_t textureUnit = 0;  // -1=environment mapping, 0=UV0, 1=UV1
        uint8_t texFlags = 0;     // M2Texture.flags (bit0=WrapS, bit1=WrapT)
        bool lanternGlowHint = false; // Texture/model hints this batch is a glow-card billboard
        bool glowCardLike = false; // Batch likely is a flat emissive card that should be sprite-replaced
        bool preserveGlowMesh = false; // Keep emissive glass/fixture mesh below its glow sprite
        // A sky model's star-point layer. Suppressed when the client draws its
        // own stars instead: the authored layer is a 256x256 compressed texture
        // magnified across the whole dome, which is soft at any resolution and
        // softer at a high one.
        bool starLayer = false;
        uint8_t glowTint = 0; // 0=warm, 1=cool, 2=red
        float batchOpacity = 1.0f; // Resolved texture weight opacity (0=transparent, skip batch)
        /// The colour alpha or texture weight changes over the model's life,
        /// so batchOpacity holds only the texture gate and the rest is sampled
        /// per instance (m2SampleBatchOpacity).
        bool animatedOpacity = false;
        /// Drawn in the blended pass because of its alpha, whatever its blend
        /// mode: the alpha animates, or is fixed below kM2BatchOpaqueOpacity.
        /// The client blends such a layer, and in that pass each instance can
        /// be faded or left out on its own.
        bool blendedByOpacity = false;
        /// The texture this batch is hidden waiting for, when its first load
        /// failed. Empty once it has it. A hidden batch is never bound, so its
        /// descriptor and material can be rewritten when a retry succeeds.
        std::string pendingTextureKey;
        float loadedOpacity = 1.0f; ///< what batchOpacity becomes once the texture arrives
        glm::vec3 center = glm::vec3(0.0f); // Center of batch geometry (model space)
        float glowSize = 1.0f;              // Approx radius of batch geometry

        struct LightBoneAnchor {
            uint16_t bone = 0;
            // Sum of (vertex position * skin weight, skin weight), divided by
            // sampled vertex count. Transforming and summing these reproduces
            // the animated center of the skinned glow geometry.
            glm::vec4 weightedPoint{0.0f};
        };
        std::vector<LightBoneAnchor> lightBoneAnchors;
        uint16_t lightSuspensionBone = UINT16_MAX;
        glm::vec3 lightSuspensionPoint{0.0f};
    };

    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    std::vector<BatchGPU> batches;
    std::vector<size_t> materialDrawOrder;

    glm::vec3 boundMin;
    glm::vec3 boundMax;
    float boundRadius = 0.0f;
    bool isSmallFoliage = false;    // Small foliage (bushes, grass, plants) - skip during taxi
    bool isInvisibleTrap = false;   // Invisible trap objects (don't render, no collision)
    bool isGroundDetail = false;    // Ground clutter/detail doodads (special fallback render path)
    bool isWaterVegetation = false; // Cattails, reeds, kelp etc. near water (insect spawning)
    bool isFireflyEffect = false;   // Firefly/fireflies M2 (exempt from particle dampeners)
    bool isWaterfall = false;       // Waterfall model (ambient sound + splash particles)
    bool isBrazierOrFire = false;   // Brazier / campfire / bonfire model
    bool isGroundFire = false;      // Ground fire whose halo follows its lowest flame emitter
    bool isTorch = false;           // Wall-mounted or standing torch
    bool isForge = false;           // Smithy forge (contained fire, lights its surroundings)
    AmbientEmitterType ambientEmitterType = AmbientEmitterType::None;

    // Collision mesh with spatial grid (from M2 bounding geometry)
    struct CollisionMesh {
        std::vector<glm::vec3> vertices;
        std::vector<uint16_t> indices;
        uint32_t triCount = 0;
        /// The box around the vertices, in model space.
        glm::vec3 boundMin{0.0f};
        glm::vec3 boundMax{0.0f};

        struct TriBounds { float minZ, maxZ; };
        std::vector<TriBounds> triBounds;

        static constexpr float CELL_SIZE = 4.0f;
        glm::vec2 gridOrigin{0.0f};
        int gridCellsX = 0, gridCellsY = 0;
        std::vector<std::vector<uint32_t>> cellFloorTris;
        std::vector<std::vector<uint32_t>> cellWallTris;

        void build();
        /// The triangles of one of the two cell arrays that a query box
        /// reaches. The two queries below differ only in which they pass.
        void gatherTrisInRange(const std::vector<std::vector<uint32_t>>& cells,
                               float minX, float minY, float maxX, float maxY,
                               std::vector<uint32_t>& out) const;

        void getFloorTrisInRange(float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;
        void getWallTrisInRange(float minX, float minY, float maxX, float maxY,
                                std::vector<uint32_t>& out) const;
        [[nodiscard]] bool valid() const { return triCount > 0; }
    };
    CollisionMesh collision;

    std::string name;

    // Skeletal animation data (kept from M2Model for bone computation)
    std::vector<pipeline::M2Bone> bones;
    std::vector<pipeline::M2Sequence> sequences;
    std::vector<uint32_t> globalSequenceDurations;  // Loop durations for global sequence tracks
    bool hasAnimation = false;  // True if any bone has keyframes
    uint32_t rtMesh = ~0u;      // RtScene mesh, or ~0u when it does not cast
    bool isSpellEffect = false;  // True for spell effect models (skip particle dampeners)
    bool disableAnimation = false; // Keep foliage/tree doodads visually stable
    bool shadowWindFoliage = false; // Apply wind sway in shadow pass for foliage/tree cards
    bool isHangingCloth = false;    // Banner/flag/tapestry: sways from the end it is held by
    bool isStandingCloth = false;   // ...and that end is the foot of a planted pole
    bool isFoliageLike = false;     // Model name matches foliage/tree/bush/grass etc (precomputed)
    bool isElvenLike = false;       // Model name matches elf/elven/quel (precomputed)
    bool isLanternLike = false;     // Model name matches lantern/lamp/light (precomputed)
    bool isLavaModel = false;       // Model name names lava: throws a local light
    bool isSkyBird = false;         // Flying bird/bat doodad - hide until animation range
    bool isLightBeam = false;       // Lighthouse/light-ray beam - distant bones must keep updating
    bool isTransportDoodad = false; // Animated ship sail/paddle child
    bool isBoat = false;            // Small boat; bobs if it turns out to be afloat
    bool hasTextureAnimation = false; // True if any batch has UV animation
    bool hasTransparentBatches = false; // True if any batch is drawn in the blended pass (blendMode >= 2 or blendedByOpacity)
    uint8_t availableLODs = 0;  // Bitmask: bit N set if any batch has submeshLevel==N

    // Particle emitter data (kept from M2Model)
    std::vector<pipeline::M2ParticleEmitter> particleEmitters;
    std::vector<VkTexture*> particleTextures;    // Resolved Vulkan textures per emitter
    std::vector<VkDescriptorSet> particleTexSets; // Pre-allocated descriptor sets per emitter (stable, avoids per-frame alloc)

    // Ribbon emitter data (kept from M2Model)
    std::vector<pipeline::M2RibbonEmitter> ribbonEmitters;
    std::vector<VkTexture*> ribbonTextures;       // Resolved texture per ribbon emitter
    std::vector<uint16_t> ribbonBlendModes;       // Blend mode of each ribbon emitter's material
    std::vector<VkDescriptorSet> ribbonTexSets;   // Descriptor sets per ribbon emitter

    /// Particles or ribbons: either needs the per-frame emitter update and
    /// an animation clock. Asking for particles alone left a model whose only
    /// emitters are ribbons with trails that were never laid.
    [[nodiscard]] bool hasEmitters() const {
        return !particleEmitters.empty() || !ribbonEmitters.empty();
    }

    /// Every texture the model's batches, emitters and ribbons point at,
    /// held for as long as the model is: those keep raw VkTexture* into the
    /// shared cache, which these references keep resident.
    std::vector<TextureRef> textureRefs;

    // Texture transform data for UV animation
    std::vector<pipeline::M2TextureTransform> textureTransforms;
    std::vector<uint16_t> textureTransformLookup;
    // Batch transparency tracks, indexed by BatchGPU::colorTrackIndex and
    // weightTrackIndex: sampled per instance for a batch whose transparency
    // animates (BatchGPU::animatedOpacity), and by the sky on its day clock.
    std::vector<pipeline::M2AnimationTrack> colorAlphaTracks;
    std::vector<pipeline::M2AnimationTrack> textureWeightTracks;
    std::vector<int> idleVariationIndices;  // Sequence indices for idle variations (animId 0)

    [[nodiscard]] bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexCount > 0; }
    /// The lights the model carries (M2 header `lights`).
    std::vector<pipeline::M2Light> lights;
    [[nodiscard]] bool canThrowLocalLight() const { return !lights.empty(); }
};

/**
 * A single M2 particle emitted from a particle emitter
 */
struct M2Particle {
    glm::vec3 position;
    glm::vec3 velocity;
    float life;        // current age in seconds
    float maxLife;     // total lifespan
    int emitterIndex;  // which emitter spawned this
    float tileIndex = 0.0f; // atlas cell picked at spawn (flag 0x10000)
    /// Its own size factor from the emitter's scale variation.
    glm::vec2 sizeFactor{1.0f};
    /// Its draws in [-1, 1] for the spin's base angle and rate, and the spin
    /// direction (flag 0x200 makes it random).
    float spinRandBase = 0.0f;
    float spinRandRate = 0.0f;
    float spinSign = 1.0f;
    /// Its own offset into the twinkle table.
    uint32_t twinkleOffset = 0;
};

/// How a particle quad is laid out (render spec, section 4.5).
enum class M2ParticleShape : uint32_t {
    Billboard = 0,      ///< facing the camera, turned by its spin
    Tail = 1,           ///< from the head back along axisA, the tail vector
    AlongVelocity = 2,  ///< facing the camera, stretched along axisA, -velocity (flag 0x4)
    Plane = 3,          ///< in the plane of axisA and axisB (flag 0x1000)
};

/// One particle quad as m2_particle.vert reads it, an instance per quad.
struct M2ParticleQuad {
    glm::vec3 position;        ///< the particle (its head), world
    glm::vec4 color;           ///< colour and alpha, straight
    glm::vec2 halfSize;        ///< half its width and height, world units
    float cell;                ///< atlas cell
    glm::vec3 axisA;           ///< per shape: tail vector, -velocity, or the plane's x axis (world)
    glm::vec3 axisB;           ///< the plane's y axis (world); unused by the other shapes
    float spin;                ///< radians, counter-clockwise, for a billboard
    uint32_t shape;            ///< M2ParticleShape
    uint32_t lit;              ///< 1: lit by the model's light (flag 0x1 clear, not Mod/Mod2x)
    InteriorLightGPU light;    ///< the model's interior light, as its batches take it
    LocalLightSet localLights = kNoLocalLights; ///< the model's point lights (local_lights.glsli)
};
static_assert(sizeof(M2ParticleQuad) == 31 * sizeof(float), "M2ParticleQuad is read as packed 32-bit values");

/**
 * Instance of an M2 model in the world
 */
struct M2Instance {
    uint32_t id = 0;     // Unique instance ID
    uint32_t modelId;
    glm::vec3 position;
    glm::vec3 rotation;  // Euler angles in degrees
    float scale;
    glm::mat4 modelMatrix;
    glm::mat4 invModelMatrix;
    // Its RtScene instance and the matrix last given to it; see syncRtScene.
    uint32_t rtInstance = ~0u;
    glm::mat4 rtMatrix{0.0f};
    glm::vec3 worldBoundsMin;
    glm::vec3 worldBoundsMax;

    // Animation state
    float animTime = 0.0f;       // Current animation time (ms)
    float globalSequenceTime = 0.0f; // Independent clock for global animation tracks
    float animSpeed = 1.0f;      // Animation playback speed
    int currentSequenceIndex = 0;// Index into sequences array
    float animDuration = 0.0f;   // Duration of current animation (ms)
    std::vector<glm::mat4> boneMatrices;

    // Idle variation state
    int idleSequenceIndex = 0;   // Default idle sequence index
    float variationTimer = 0.0f; // Time until next variation attempt (ms)
    bool playingVariation = false;// Currently playing a one-shot variation

    /// Stop at the end of the sequence and stay there.
    ///
    /// A door is a pose, not a performance: the server says open or closed and
    /// the model holds that. Without this a one-shot ran to its end and went
    /// back to the idle sequence, which for a door is the closed pose - so an
    /// open door swung open and shut itself, over and over.
    bool holdAtEnd = false;
    /// Played when the current one-shot ends: a GameObject's Opened after
    /// its Open. A negative animSpeed plays the current one backwards, and
    /// then it ends at its start.
    std::optional<M2AnimationPlay> nextAnimation;
    /// Ended by finishInstance: its mesh is not drawn and its emitters stop,
    /// while the particles it made fly out their lives.
    bool finishing = false;
    /// Out of the world for now; see M2Renderer::setInstanceHidden.
    bool hidden = false;

    /// Lit while the player is pressing on this instance: 0 none, 1 full.
    ///
    /// A game object has no selection circle - it is used rather than selected
    /// - so the press is the only thing that can say the click landed on this
    /// one and not the scenery beside it.
    float highlight = 0.0f;
    /// An alpha its owner sets on the whole model - the sky's crossfade
    /// between zones. Below one the instance is drawn in the blended pass
    /// alone, every layer of it, since an opaque layer cannot fade otherwise.
    float fade = 1.0f;

    // Particle emitter state
    std::vector<float> emitterAccumulators;  // fractional particle counter per emitter
    /// Per emitter, the most particles it may hold (m2EmitterPool): grown to
    /// what it has needed, never shrunk, as the client keeps it.
    std::vector<uint32_t> emitterPools;
    std::vector<M2Particle> particles;
    /// How far its live particles are from cachedCullCenter, at most, as of
    /// the last update: with the model's own radius, the sphere they are seen
    /// in. A mage portal's swirl and a tail of smoke leave the model's bounds.
    float particleReach = 0.0f;

    // Ribbon emitter state, kept the way the original client's CRibbonEmitter
    // keeps it. An edge is two world-space vertices laid across the emitter's
    // own Y axis - bone and model orientation included - at the moment it is
    // born, and left where it was put but for gravity. A trail from a turning
    // bone therefore twists, as the phoenix's wing trails do.
    struct RibbonEdge {
        glm::vec3 below{0.0f};  // heightBelow down the emitter's Y axis
        glm::vec3 above{0.0f};  // heightAbove up it
        glm::vec3 color{1.0f};
        float alpha = 1.0f;
        float age = 0.0f;       // seconds; the texture's u runs with it
    };
    struct RibbonState {
        std::deque<RibbonEdge> edges;  // oldest first
        /// The edge at the emitter now, laid again every update and drawn
        /// after the rest.
        RibbonEdge head;
        bool hasHead = false;
        /// Where the emitter was at the last update: the new edges are laid
        /// along the curve from there to here.
        bool posSet = false;
        glm::vec3 lastPos{0.0f};
        glm::vec3 lastDir{0.0f};
        glm::vec3 lastVertical{0.0f};
        /// The fraction of an edge carried to the next update.
        float startTime = 0.0f;
    };
    std::vector<RibbonState> ribbons;  // one per ribbon emitter

    // Cached model flags (set at creation to avoid per-frame hash lookups)
    bool cachedHasAnimation = false;
    bool cachedDisableAnimation = false;
    bool cachedHasEmitters = false;  // M2ModelGPU::hasEmitters
    bool cachedIsGroundDetail = false;
    bool cachedIsInvisibleTrap = false;
    bool cachedIsSkyBird = false;
    bool cachedIsLightBeam = false;
    bool cachedIsTransportDoodad = false;
    bool cachedIsBoat = false;
    // Whether the boat sits on water: -1 not known yet, 0 no, 1 afloat.
    int8_t afloat = -1;
    uint8_t afloatTries = 0;
    float afloatRecheck = 0.0f;
    bool cachedIsValid = false;
    bool skipCollision = false;    // Fully non-collidable visual/effect instance
    bool skipWallCollision = false; // Keep authored floors, suppress only wall blocking
    /// The light of the WMO interior group the instance stands in, if any.
    InteriorLightGPU interiorLight;
    float cachedBoundRadius = 0.0f;
    glm::vec3 cachedCullCenter{0.0f};              // transformed visual-bounds center
    float cachedVisualRadius = 0.0f;               // transformed visual-bounds half diagonal
    // Pre-computed per-instance cull factors (depend only on static flags + scale +
    // bound radius), populated by recomputeCachedCullFactors(). The per-frame SSBO
    // upload just multiplies by the smoothed render distance and packs the rest.
    float cachedEffectiveMaxDistSqFactor = 1.0f;  // multiplied by maxRenderDistanceSq each frame
    float cachedPaddedRadius = 0.0f;              // sphere radius used by the cull compute
    const M2ModelGPU* cachedModel = nullptr;  // Avoid per-frame hash lookups

    // Result of the most recent GPU cull dispatch that actually covered this
    // instance, matched back by instance ID (never by array index - see
    // cullSubmittedIds_).  Defaults to visible so an instance created after the
    // in-flight dispatch draws immediately instead of waiting ~2 frames for its
    // first cull result.
    uint8_t lastCullVisible = 1;
    /// The point lights picked for this instance this frame (local_lights.hpp).
    LocalLightSet frameLocalLights = kNoLocalLights;
    // Consecutive frames this instance has been culled (capped at 3), used for
    // the HiZ `previouslyVisible` hysteresis.  Defaults to 2 ("not recently
    // visible") so a freshly spawned instance skips the HiZ test rather than
    // being judged against depth data that never contained it.
    uint8_t hizPrevCulledFrames = 2;
    // Which render() call last scattered a GPU verdict onto this instance.
    // Only a verdict from the call reading it counts: the dispatch covers the
    // instances the camera's columns held then, not a fixed prefix of the array.
    uint32_t cullVerdictSerial = 0;
    // Server game object (mailbox, chest, node, door): exempt from the adaptive
    // ambient-doodad distance, which collapses to ~200 units in a city and hid
    // interactable objects long before the server stopped sending them.
    // Set by the spawner; see M2_GAME_OBJECT_MIN_RENDER_DISTANCE.
    bool isGameObject = false;

    void recomputeCachedCullFactors();

    // Frame-skip optimization (update distant animations less frequently)
    uint8_t frameSkipCounter = 0;
    bool bonesDirty[2] = {false, false};  // Per-frame-index: set when bones recomputed, cleared after upload
    // Mega-bone slot this instance's bones were last uploaded to, per frame
    // index (0 = never uploaded; valid slots start at 1). Lets prepareRender
    // skip the memcpy when the bones are unchanged and still at the same slot.
    uint32_t megaBoneUploadedSlot[2] = {0, 0};

    // Per-instance bone SSBO (double-buffered) - legacy; see mega bone SSBO in M2Renderer
    ::VkBuffer boneBuffer[2] = {};
    VmaAllocation boneAlloc[2] = {};
    void* boneMapped[2] = {};
    VkDescriptorSet boneSet[2] = {};

    // Mega bone SSBO offset - base bone index for this instance (set per-frame in prepareRender)
    uint32_t megaBoneOffset = 0;

    void updateModelMatrix();
};


// M2 material UBO - matches M2Material in m2.frag.glsl (set 1, binding 2)
struct M2MaterialUBO {
    int32_t hasTexture;
    int32_t alphaTest;
    int32_t unlit;
    int32_t blendMode;
    int32_t cullBackFaces; // authored single-sided transparent material
    float specularIntensity;
    float emissiveBoost;
    // The batch's authored colour, from the M2's colour track, with any
    // flicker already folded in. Three scalars rather than a vec3 so the
    // std140 rules stay trivial.
    float tintR;
    float tintG;
    float tintB;
    /// M2Combiner: how the stages and the lit colour meet.
    int32_t combiner;
    /// The second stage is bound; without it the combiner reads it as white.
    int32_t hasSecondTexture;
};

// M2 params UBO - matches M2Params in m2.vert.glsl (set 1, binding 1)
struct M2ParamsUBO {
    float uvOffsetX;
    float uvOffsetY;
    int32_t texCoordSet;
    int32_t useBones;
};

/**
 * M2 Model Renderer (Vulkan)
 *
 * Handles rendering of M2 models (doodads like trees, rocks, bushes)
 */
class M2Renderer {
    struct InstanceState;
public:
    /// The owning scene holds this lease. Geometry/texture caches stay here;
    /// instances, collision indexes and incomplete uploads die with the lease.
    class InstanceSession {
    public:
        explicit InstanceSession(M2Renderer& renderer);
        ~InstanceSession();
        InstanceSession(const InstanceSession&) = delete;
        InstanceSession& operator=(const InstanceSession&) = delete;
    private:
        M2Renderer& renderer_;
        std::unique_ptr<InstanceState> state_;
    };
    M2Renderer();
    ~M2Renderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assets);
    /** Configure this renderer for camera-centered sky M2s before initialize(). */
    void setSkyMode(bool enabled) { skyMode_ = enabled; }
    void setSkyTimeOfDay(float hours) { skyTimeOfDayHours_ = hours; }
    void shutdown();

    [[nodiscard]] bool hasModel(uint32_t modelId) const;
    bool loadModel(const pipeline::M2Model& model, uint32_t modelId);
    /** Force-remove a model and all its GPU resources. Caller must ensure no instances reference it. */
    void unloadModel(uint32_t modelId);
    /** Mark a loaded model as a spell effect (full-brightness particles, no collision). */
    void markModelAsSpellEffect(uint32_t modelId);

    // ---- Emitters of a model another renderer draws and poses ----
    //
    // A creature or a mount is drawn by CharacterRenderer, which has no
    // particles or ribbons; its emitters run here, on a skeleton posed there.
    // Such emitters live apart from the world's instances: they have no mesh,
    // no animation of their own, no culling entry, no collision and no place
    // in the ray-traced scene.

    /// What the owner of the skeleton says about it this frame. `bones` are
    /// in the same model-space convention as computeBoneMatrices; an empty
    /// list leaves every bone at rest. Not `posed`: the owner did not move the
    /// skeleton this frame (out of its animation range, or hidden), so
    /// nothing new is emitted and what is out there plays out.
    struct EmitterPose {
        glm::mat4 modelMatrix{1.0f};
        float scale = 1.0f;
        const std::vector<glm::mat4>* bones = nullptr;
        int sequenceIndex = 0;
        float animationTimeMs = 0.0f;
        float globalSequenceTimeMs = 0.0f;
        bool posed = false;
    };
    /// Uploads `model`'s particle and ribbon emitters - their textures and
    /// descriptor sets, not its geometry. Returns a handle, or 0 when the
    /// model has no emitters.
    uint32_t loadPosedEmitterModel(const pipeline::M2Model& model);
    void unloadPosedEmitterModel(uint32_t handle);
    [[nodiscard]] bool hasPosedEmitterModel(uint32_t handle) const;
    /// One set of emitters on one skeleton. 0 when the handle is not loaded.
    uint32_t createPosedEmitters(uint32_t modelHandle);
    void removePosedEmitters(uint32_t id);
    [[nodiscard]] bool hasPosedEmitters(uint32_t id) const;
    void setPosedEmittersPose(uint32_t id, const EmitterPose& pose);
    /// Emits and moves the particles and ribbons of every posed set. Runs on
    /// the main thread once the frame's skeletons are final - after both
    /// update() and the owner's own update have returned.
    void updatePosedEmitters(float dt);

    /// Instances of the same model at the same spot are treated as one placement,
    /// which is right for the static world and wrong for anything whose position
    /// arrives later from a parent. Pass allowPositionDedup=false for a child
    /// instance created at a placeholder position - see the note in
    /// setInstanceTransform on why the placeholder key outlives the placement.
    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                            const glm::vec3& rotation = glm::vec3(0.0f),
                            float scale = 1.0f,
                            bool allowPositionDedup = true);
    uint32_t createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                       const glm::vec3& position);

    /// Animate, emit and cull for this frame's camera: its view (which
    /// billboard bones turn against) and projection.
    void update(float deltaTime, const glm::mat4& view, const glm::mat4& projection);

    /**
     * Render all visible instances (Vulkan)
     */
    /** Pre-allocate GPU resources (bone SSBOs, descriptors) on main thread before parallel render. */
    void prepareRender(uint32_t frameIndex, const Camera& camera);
    /** Dispatch GPU frustum culling compute shader on primary cmd before render pass. */
    void dispatchCullCompute(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera);
    /// The opaque pass, and the blended one too unless it is drawn after the
    /// water (see renderAfterWater).
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

    /// Draw blended things - transparent batches, glow sprites, particles,
    /// and ribbons - in the water's pass, after the water, instead of in
    /// the scene pass under it. `pass` is that pass, or null while the water is
    /// drawn in the scene pass. Rebuilds pipelines: call between frames, with
    /// nothing in flight using the old ones.
    void setAfterWaterPass(VkRenderPass pass, VkSampleCountFlagBits samples);
    [[nodiscard]] VkRenderPass afterWaterPass() const { return afterWaterPass_; }
    /// Whether render() leaves the blended things for renderAfterWater.
    [[nodiscard]] bool drawsBlendedAfterWater() const {
        return afterWaterPass_ != VK_NULL_HANDLE && afterWaterPipes_.alpha != VK_NULL_HANDLE;
    }
    /// Record them, into the water's pass after the water. Needs render() to
    /// have run this frame.
    void renderAfterWater(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /// Append the point lights of the models animated this frame - those in
    /// view, and those within ten yards of the camera - as the client lights
    /// a scene with them (local_lights.hpp).
    void gatherLocalLights(const glm::vec3& cameraPos, std::vector<LocalLight>& out) const;
    /// The frame's point lights, which every draw picks its nearest from.
    /// Owned by the Renderer, filled before anything draws.
    void setFrameLocalLights(const std::vector<LocalLight>* lights) { frameLights_ = lights; }

    /** Set the HiZ system for occlusion culling (Phase 6.3). nullptr disables HiZ. */
    void setHiZSystem(HiZSystem* hiz) { hizSystem_ = hiz; }

    /// Where loaded models register their geometry for the ray traced
    /// lighting. Static doodads only - animated models would cast their
    /// bind pose. Not given to the sky's renderer.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }
    /// Water surface height near a point, for telling a moored boat from a beached one.
    void setWaterHeightQuery(std::function<std::optional<float>(float, float, float)> q) { waterHeightAt_ = std::move(q); }
    /// Bring the scene's instances in line with this renderer's, once a frame
    /// while the lighting is on.
    void syncRtScene();
    void setForceNoCull(bool v) { forceNoCull_ = v; }

    /** Ensure GPU→CPU cull output is visible to the host after a fence wait.
     *  Call after the early compute submission finishes (endSingleTimeCommands). */
    void invalidateCullOutput(uint32_t frameIndex);

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);
    [[nodiscard]] bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    /**
     * Render depth-only pass for shadow casting. `wind` is the frame's wind,
     * the same numbers the main pass reads from GPUPerFrameData, so a tree's
     * shadow bends with the tree.
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, const WindGpu& wind,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f,
                      bool resetDescriptors = true);

    /**
     * Render M2 particle emitters (point sprites)
     */
    void renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /**
     * Render M2 ribbon emitters (spell trails / wing effects)
     */
    void renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    /// WOWEE_M2_CENSUS: what a model is actually drawn at, said once each.
    void censusInstance(const M2Instance& instance);

    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
    void setInstanceAnimationFrozen(uint32_t instanceId, bool frozen);
    /// Play from the first frame, with bones to match.
    ///
    /// New instances start at a random point so placed doodads don't animate
    /// in lockstep, and take their first bones from a sibling that is already
    /// mid-animation. A spell cast has to start at its beginning instead.
    void restartInstanceAnimation(uint32_t instanceId);
    /// Play an animation once and stay on its last frame.
    ///
    /// skipToEnd puts the model there immediately, which is what a door that
    /// was already open when it came into view wants - the swing belongs to
    /// the moment it opens, not to the moment it is first seen.
    void setInstanceAnimationHeld(uint32_t instanceId, uint32_t animationId,
                                  bool skipToEnd);
    /// Play an animation as a plan says - forwards or back, held, looped,
    /// or followed by another. False when the model has no such sequence.
    bool playInstanceAnimation(uint32_t instanceId, const M2AnimationPlay& now,
                               const std::optional<M2AnimationPlay>& next = std::nullopt);

    /// Light an instance while it is being pressed on. 0 clears it.
    void setInstanceHighlight(uint32_t instanceId, float amount);
    /// See M2Instance::fade.
    void setInstanceFade(uint32_t instanceId, float alpha);
    /// Take an instance out of the world without removing it, or put it back.
    ///
    /// A hidden instance is not drawn in any pass, is not handed to the GPU
    /// cull, lights nothing, emits no particles or ribbons - what it had out
    /// goes with it - and is no floor or wall. Its clock stops and its
    /// transform still follows its owner, so shown again it carries on from
    /// where it was. A ship's sails are hidden with the hull whose route is on
    /// the other map (WMORenderer::setInstanceHidden).
    void setInstanceHidden(uint32_t instanceId, bool hidden);
    void setInstancesHidden(const std::vector<uint32_t>& instanceIds, bool hidden);
    /// Take the light off whatever has it, whichever instance that was.
    void clearInstanceHighlights();
    /// Set the animation sequence by animation ID (e.g. anim::OPEN, anim::CLOSE).
    /// Finds the first sequence with matching ID. Unfreezes the instance and resets time.
    void setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true);
    /// Check if a model instance has a specific animation ID in its sequence table.
    [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
    [[nodiscard]] float getInstanceAnimDuration(uint32_t instanceId) const;
    /// World-space visual bounding sphere of an instance (center + radius),
    /// used for cursor picking and selection circles. Returns false for an
    /// unknown instance or a degenerate (zero-radius) model.
    bool getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const;

    /// The world-space box of an instance's collision geometry.
    ///
    /// What a lift's deck actually covers, as opposed to how far its origin is
    /// from you. Boarding a generic M2 transport is decided on a radius around
    /// that origin, which is why standing beside an Undercity lift shaft
    /// attaches you to the car.
    bool getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin, glm::vec3& outMax) const;

    /// The floor one named instance offers under a point, ignoring every other.
    ///
    /// Asked of a lift car, this is "is its deck under your feet" - which a box
    /// around the car cannot answer, because an axis-aligned box covers the
    /// doorway and the ground just outside it.
    std::optional<float> getInstanceFloorHeight(uint32_t instanceId,
                                                float glX, float glY, float glZ) const;

    /// True while the instance is still live in the renderer. Owners that cache
    /// instance IDs (game objects, transports) use this to notice an instance
    /// that was dropped underneath them - e.g. by a renderer-wide clear - and
    /// respawn it instead of silently addressing a dead handle forever.
    [[nodiscard]] bool hasInstance(uint32_t instanceId) const {
        return scene_->instanceIndexById.find(instanceId) != scene_->instanceIndexById.end();
    }
    void removeInstance(uint32_t instanceId);
    void removeInstances(const std::vector<uint32_t>& instanceIds);
    /// End an effect as the client does (CEffect::Finish, WoW.exe 0x6f87c0):
    /// its mesh goes at once, its emitters stop, and the particles and ribbons
    /// it has already made fly out their lives; then it is removed.
    void finishInstance(uint32_t instanceId);
    /// Mark an instance as a server game object so the adaptive doodad render
    /// distance can't cull it while the server still considers it visible.
    void setInstanceIsGameObject(uint32_t instanceId, bool isGameObject);
    void setSkipCollision(uint32_t instanceId, bool skip);
    void setSkipWallCollision(uint32_t instanceId, bool skip);
    /// Light the instance as standing inside a WMO interior group, or as
    /// outdoors when `light` has no value.
    void setInteriorLight(uint32_t instanceId, const std::optional<InteriorLight>& light);
    void clear();
    /** Drop all instances but keep models in GPU memory. Cheap path for the
     *  editor's rebuild loop where the same model is re-instanced repeatedly. */
    void clearInstances();
    void cleanupUnusedModels();
    /// Keep a model resident even while it has no instances. Game object models
    /// are a small, bounded set (one per display ID actually encountered) that
    /// goes instance-free every time the player leaves an area and comes back -
    /// exactly the reap/reload churn seen in the field. Pinning them keeps that
    /// cycle out of the picture entirely; ambient doodads still get reaped.
    void setModelPinned(uint32_t modelId, bool pinned);
    /** Return and clear the model IDs evicted by cleanupUnusedModels() since the
     *  last call. Lets callers that track uploaded models drop stale entries. */
    std::vector<uint32_t> drainReapedModelIds();

    bool checkCollision(const glm::vec3& from, const glm::vec3& to,
                        glm::vec3& adjustedPos, float playerRadius = 0.5f) const;
    std::optional<float> getFloorHeight(float glX, float glY, float glZ, float* outNormalZ = nullptr) const;

    /// Every doodad the floor query would consider here, and what each one
    /// contributes. The WMO side has had debugDumpGroupsAtPosition for this
    /// since the Undercity pull was first chased; the M2 side had nothing, so
    /// a floor that should have come from a lift deck and did not left no
    /// trace at all beyond "m2=-99999".
    void debugDumpFloorCandidatesAt(float glX, float glY, float glZ) const;
    /// How far along a ray the first doodad collision triangle lies, up to
    /// maxDistance: the camera's doodad collision.
    [[nodiscard]] float raycastCollision(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;
    void setCollisionFocus(const glm::vec3& worldPos, float radius);

    void resetQueryStats();
    [[nodiscard]] double getQueryTimeMs() const { return queryTimeMs; }
    [[nodiscard]] uint32_t getQueryCallCount() const { return queryCallCount; }

    void recreatePipelines();

    /// Build the main-pass pipelines. Called by initialize() and again by
    /// recreatePipelines() after a device loss, which is the reason it exists:
    /// the two used to be separate copies of the same 190 lines.
    bool buildMainPassPipelines(VkDescriptorSetLayout perFrameLayout);
    VkPipeline blendPipeline(M2BlendPipeline kind) const {
        switch (kind) {
            case M2BlendPipeline::Opaque: return opaquePipeline_;
            case M2BlendPipeline::AlphaTest: return alphaTestPipeline_;
            case M2BlendPipeline::Alpha: return blended().alpha;
            case M2BlendPipeline::NoAlphaAdd: return blended().noAlphaAdd;
            case M2BlendPipeline::Modulate: return blended().modulate;
            case M2BlendPipeline::Modulate2x: return blended().modulate2x;
            case M2BlendPipeline::AlphaAdd: break;
        }
        return blended().additive;
    }
    /// Decide how a batch is drawn and write its material buffer. Runs when the
    /// model loads, and again if it is marked a spell effect afterwards; the
    /// draw loops write nothing into it.
    void writeBatchMaterial(const M2ModelGPU& model, M2ModelGPU::BatchGPU& batch) const;
    /// The texture as one batch samples it: its image, with a sampler built
    /// from that batch's own wrap flags.
    VkDescriptorImageInfo batchImageInfo(const VkTexture& texture, uint8_t texFlags) const;
    /// Take what the loaded texture says about itself onto the batch.
    void applyBatchTextureProps(M2ModelGPU::BatchGPU& batch, const TextureInfo& texture) const;
    /// Point a batch's descriptor set at its texture.
    void writeBatchDescriptors(M2ModelGPU::BatchGPU& batch) const;
    /// Ask again for textures that failed to load, and show the batches that
    /// were hidden waiting for them. A failed MPQ read is often transient.
    void retryPendingBatchTextures();
    struct PendingBatchTexture { uint32_t modelId; size_t batchIndex; };
    std::vector<PendingBatchTexture> pendingBatchTextures_;

    // Stats
    [[nodiscard]] bool isInitialized() const { return initialized_; }
    [[nodiscard]] uint32_t getModelCount() const { return static_cast<uint32_t>(models.size()); }
    [[nodiscard]] uint32_t getInstanceCount() const { return static_cast<uint32_t>(scene_->instances.size()); }
    [[nodiscard]] uint32_t getTotalTriangleCount() const;
    [[nodiscard]] uint32_t getDrawCallCount() const { return lastDrawCallCount; }

    /**
     * Append the 2D footprints of placed props touching the given window,
     * for the grass generator's clearings. Ground detail is skipped - it is
     * the clutter itself - and the footprint radius is capped so a tree
     * clears the ground at its trunk, not under its whole crown.
     */
    void collectGrassClearings(float minX, float minY, float maxX, float maxY,
                               std::vector<pipeline::GrassClearingSource>& out) const;

    // Lighting/fog/shadow are now in per-frame UBO; these are no-ops for API compat
    void setFog(const glm::vec3& /*color*/, float /*start*/, float /*end*/) {}
    void setLighting(const float /*lightDirIn*/[3], const float /*lightColorIn*/[3],
                     const float /*ambientColorIn*/[3]) {}
    void setShadowMap(uint32_t /*depthTex*/, const glm::mat4& /*lightSpace*/) {}
    void clearShadowMap() {}

    /// How far the furthest doodad drawn this frame was. See
    /// Renderer::logViewDistanceDiag.
    [[nodiscard]] float getFurthestDrawnDistance() const { return std::sqrt(furthestDrawnSq_); }
    /// How many particles to emit, as a fraction of what a model asks for -
    /// the game's Particle Density.
    ///
    /// Applied to the authored rate before the floor that holds flames
    /// together, so turning it down thins smoke, dust and spell effects while
    /// a candle keeps enough particles alive to still read as a flame. See the
    /// note in m2_renderer_particles.cpp: below that floor a fire does not get
    /// smaller, it disappears.
    void setParticleDensity(float density) {
        particleDensity_ = std::clamp(density, 0.05f, 1.0f);
    }
    [[nodiscard]] float particleDensity() const { return particleDensity_; }

    void setSuppressBakedStars(bool suppress) { suppressBakedStars_ = suppress; }

    /// Drop the ground clutter from this pass, for the ablation's clutter
    /// phase. Clutter is drawn by the thousand and shares this renderer with
    /// the world's doodads, so switching the whole pass off says what the two
    /// cost together and nothing about which of them it was.
    void setSkipGroundDetail(bool skip) { skipGroundDetail_ = skip; }

    /// Hold the doodads to this many yards, on top of whatever the density
    /// rule and the instance's own size allow. Zero lifts it.
    ///
    /// The ablation's far-doodad phase: these models have no LOD skins, so a
    /// tree a thousand yards off is drawn with every triangle it has to cover
    /// a few pixels. Whether that is where the time goes is a question about
    /// distance alone, and switching the whole pass off cannot answer it.
    void setDoodadDistanceCap(float yards) { doodadDistanceCapYards_ = yards; }

    /// The doodad view distance with that cap applied.
    float cappedViewDistance() const {
        return doodadDistanceCapYards_ > 0.0f
                   ? std::min(viewDistanceAbsolute_, doodadDistanceCapYards_)
                   : viewDistanceAbsolute_;
    }
    void setViewDistance(float distance) {
        viewDistanceRaw_ = std::clamp(distance, 400.0f, 2400.0f);
        // And the distance itself, as a ceiling. The scale multiplies a
        // constant tuned for scene density - 2800 yards where models are
        // sparse - so at 2400 it put doodads 5600 yards out while the terrain
        // and the WMOs both stop at the 2400 the player asked for. Distant
        // trees and buildings then stood on nothing.
        viewDistanceAbsolute_ = viewDistanceRaw_;
        recomputeViewDistanceScale();
    }

    /// How far the world's clutter is drawn, against how far the world is -
    /// the game's Environment Detail.
    ///
    /// View distance is how far there is anything to see; this is how much of
    /// the scenery inside that is worth drawing, and it only ever takes away.
    /// Above 1 it changes nothing: the ceiling above is the terrain's own, and
    /// a doodad past that is a tree standing on nothing, which is the fault
    /// that ceiling was added for.
    /// How far the ground cover is drawn - the game's Ground Clutter Radius.
    ///
    /// Its own distance rather than a share of the doodad one: grass stops
    /// between 70 and 140 yards in the original client while doodads run to
    /// the horizon, and the setting names those yards outright.
    void setGroundDetailDistance(float yards) {
        groundDetailMaxDistance_ = std::clamp(yards, 0.0f, 500.0f);
    }
    [[nodiscard]] float groundDetailDistance() const { return groundDetailMaxDistance_; }

    void setEnvironmentDetail(float detail) {
        environmentDetail_ = std::clamp(detail, 0.25f, 1.5f);
        recomputeViewDistanceScale();
    }
    [[nodiscard]] float environmentDetail() const { return environmentDetail_; }

    [[nodiscard]] std::vector<glm::vec3> getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const;

    // Pre-decoded BLP cache: set by terrain manager before calling loadModel()
    // so loadTexture() can skip the expensive assetManager->loadTexture() call.
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

private:
    bool initialized_ = false;
    pipeline::AssetManager* assetManager = nullptr;

    // Vulkan context
    VkContext* vkCtx_ = nullptr;

    // Vulkan pipelines (one per blend mode)
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;       // blend mode 0
    /// Cutout: leaves, ground clutter, anything the fragment shader alpha-tests.
    /// Blend disabled with alpha-to-coverage on, which is what turns the
    /// shader's sharpened alpha into per-sample coverage.
    VkPipeline cutoutPipeline_ = VK_NULL_HANDLE;
    /// The ablation's clutter phase; see setSkipGroundDetail.
    bool skipGroundDetail_ = false;
    /// The ablation's far-doodad phase; see setDoodadDistanceCap.
    float doodadDistanceCapYards_ = 0.0f;
    /// Said once: a foliage batch reached the shadow pass with no texture.
    bool warnedShadowNoTexture_ = false;
    /// Foliage casters drawn into the shadow map last frame, for spotting a
    /// caster count that swings while nothing is moving.
    uint32_t lastFoliageCasters_ = 0;
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;     // blend mode 1
    /// Everything drawn blended: the transparent batches, particles and
    /// ribbons. They exist twice. One set is for the scene pass. The other is
    /// for the pass the water is drawn in, when the water leaves the scene
    /// pass: blended things drawn before the water had the water's sheet
    /// laid over them, so a waterfall's foam showed at full strength only
    /// where it hung past the pool.
    struct BlendedPipelines {
        VkPipeline alpha = VK_NULL_HANDLE;            // blend mode 2
        VkPipeline noAlphaAdd = VK_NULL_HANDLE;       // blend mode 3
        VkPipeline additive = VK_NULL_HANDLE;         // blend mode 4 and up past 6
        VkPipeline modulate = VK_NULL_HANDLE;         // blend mode 5
        VkPipeline modulate2x = VK_NULL_HANDLE;       // blend mode 6
        /// M2 emitter particles, one per blend pipeline (m2BlendPipeline of
        /// the emitter's blend mode). AlphaAdd also draws the glow sprites.
        std::array<VkPipeline, kM2BlendPipelineCount> particle{};
        VkPipeline ribbon = VK_NULL_HANDLE;           // alpha-blended ribbons
        VkPipeline ribbonAdditive = VK_NULL_HANDLE;   // additive ribbons
    };
    BlendedPipelines scenePipes_;
    BlendedPipelines afterWaterPipes_;
    VkRenderPass afterWaterPass_ = VK_NULL_HANDLE;
    /// Set while renderAfterWater records, so the blended draws pick the
    /// pipelines built for the water's pass.
    bool drawingAfterWater_ = false;
    /// render() left its blended half for renderAfterWater this frame.
    bool blendedPendingAfterWater_ = false;
    const BlendedPipelines& blended() const {
        return drawingAfterWater_ ? afterWaterPipes_ : scenePipes_;
    }
    bool buildBlendedPipelines(VkRenderPass pass, VkSampleCountFlagBits samples,
                               bool derive, BlendedPipelines& out);
    void destroyBlendedPipelines(BlendedPipelines& pipes);
    /// Pass 2: the transparent batches and the glow sprites, after the opaque
    /// pass has culled, sorted and gathered them.
    void recordTransparentPass(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering (Phase 7)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;
    // Per-frame pools for foliage shadow texture descriptor sets (one per frame-in-flight)
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};

    // Particle pipelines
    VkPipelineLayout particlePipelineLayout_ = VK_NULL_HANDLE;

    // Ribbon pipelines (additive + alpha-blend)
    VkPipelineLayout ribbonPipelineLayout_ = VK_NULL_HANDLE;
    /// The per-frame set layout initialize() was given. recreatePipelines()
    /// runs long after that call and needs the same one.
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;      // set 2
    VkDescriptorSetLayout particleTexLayout_ = VK_NULL_HANDLE;  // particle set 1 (texture only)

    // Descriptor pools
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    std::shared_ptr<std::atomic<uint64_t>> boneDescPoolGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    static constexpr uint32_t MAX_MATERIAL_SETS = 16384;
    static constexpr uint32_t MAX_BONE_SETS = 16384;

    // Dummy identity bone buffer + descriptor set for non-animated models.
    // The pipeline layout declares set 2 (bones) and some drivers (Intel ANV)
    // require all declared sets to be bound even when the shader doesn't access them.
    ::VkBuffer dummyBoneBuffer_ = VK_NULL_HANDLE;
    VmaAllocation dummyBoneAlloc_ = VK_NULL_HANDLE;
    VkDescriptorSet dummyBoneSet_ = VK_NULL_HANDLE;

    // Mega bone SSBO - consolidates all per-instance bone matrices into a single buffer per frame.
    // Replaces per-instance bone SSBOs for fewer descriptor binds and enables GPU instancing.
    static constexpr uint32_t MEGA_BONE_MAX_INSTANCES = 4096;
    // Per-instance bone ranges are packed at their model's true bone count
    // rather than at a fixed stride, so the buffer is sized in matrices. The
    // budget matches the old 4096 x 128 slots; models under the old stride
    // now leave room for the ones above it.
    static constexpr uint32_t MEGA_BONE_MATRIX_CAPACITY = MEGA_BONE_MAX_INSTANCES * 128;
    ::VkBuffer megaBoneBuffer_[2] = {};
    VmaAllocation megaBoneAlloc_[2] = {};
    void* megaBoneMapped_[2] = {};
    VkDescriptorSet megaBoneSet_[2] = {};

    // GPU instance data SSBO - per-instance transforms, fade, bones for instanced draws.
    // Shader reads instanceData[push.instanceDataOffset + gl_InstanceIndex].
    // Everything that differs between instances of one batch lives here, not
    // in the batch's material buffer: that one buffer is shared by every
    // instance and read when the GPU executes, so what was written into it
    // during recording reached all of them as whatever the last write said.
    struct M2InstanceGPU {
        glm::mat4 model;           // 64 bytes @ offset 0
        M2UVTransform uv;          // 32 bytes @ offset 64 - texture animation, stage 0
        M2UVTransform uv2;         // 32 bytes @ offset 96 - texture animation, mask stage
        float fadeAlpha;           //  4 bytes @ offset 128
        int32_t useBones;          //  4 bytes @ offset 132
        int32_t boneBase;          //  4 bytes @ offset 136
        int32_t boneCount;         //  4 bytes @ offset 140 - clamps skinning reads
        float highlight = 0.0f;    //  4 bytes @ offset 144 - pressed-on lift
        int32_t flags = {};        //  4 bytes @ offset 148 - bit 0: afloat (bobs on the water)
        float pad1 = 0.0f;         //  4 bytes @ offset 152
        float pad0 = 0.0f;         //  4 bytes @ offset 156
        InteriorLightGPU interiorLight; // 32 bytes @ offset 160 - M2Instance::interiorLight
        LocalLightSet localLights = kNoLocalLights; // 16 bytes @ offset 192 - local_lights.glsli
    };
    // How many instances one frame may hand the GPU, not how many exist. Ground
    // clutter is what fills it: it is drawn by the thousand and every tuft
    // takes a slot of its own, so at 16384 a dwarf standing in Dun Morogh
    // grass was already losing whole models for a frame at a time - which reads
    // as clutter blinking rather than as anything being over budget. Two
    // buffers of 208 bytes a slot, so this costs 13 MB.
    static constexpr uint32_t MAX_INSTANCE_DATA = 32768;
    VkDescriptorSetLayout instanceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool instanceDescPool_ = VK_NULL_HANDLE;
    ::VkBuffer instanceBuffer_[2] = {};
    VmaAllocation instanceAlloc_[2] = {};
    void* instanceMapped_[2] = {};
    VkDescriptorSet instanceSet_[2] = {};
    uint32_t instanceDataCount_ = 0; // reset each frame in render()

    // GPU Frustum Culling via Compute Shader
    // Compute shader tests each M2 instance against frustum planes + distance, writes visibility[].
    // CPU reads back visibility to build sortedVisible_ without per-instance frustum/distance tests.
    struct CullInstanceGPU {        // matches CullInstance in m2_cull.comp.glsl (32 bytes, std430)
        glm::vec4 sphere;           // xyz = world position, w = padded radius
        float effectiveMaxDistSq;   // adaptive distance cull threshold
        uint32_t flags;             // bit 0 = valid, bit 1 unused, bit 2 = invisibleTrap, bit 3 = previously visible
        float _pad[2] = {};
    };
    struct CullUniformsGPU {        // matches CullUniforms in m2_cull_hiz.comp.glsl (std140)
        glm::vec4 frustumPlanes[6]; // xyz = normal, w = distance         (96 bytes)
        glm::vec4 cameraPos;        // xyz = camera position, w = maxPossibleDistSq (16 bytes)
        uint32_t instanceCount;     //                                    (4 bytes)
        uint32_t hizEnabled;        // 1 = HiZ occlusion active           (4 bytes)
        uint32_t hizMipLevels;      // mip levels in HiZ pyramid          (4 bytes)
        uint32_t _pad2 = {};        //                                    (4 bytes)
        glm::vec4 hizParams;        // x=pyramidW, y=pyramidH, z=nearPlane, w=unused (16 bytes)
        glm::mat4 viewProj;         // current frame view-projection                 (64 bytes)
        glm::mat4 prevViewProj;     // previous frame VP for HiZ reprojection        (64 bytes)
    };                              // Total: 272 bytes
    static constexpr uint32_t MAX_CULL_INSTANCES = 24576;
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;           // frustum-only (fallback)
    VkPipeline cullHiZPipeline_ = VK_NULL_HANDLE;        // frustum + HiZ occlusion
    VkPipelineLayout cullPipelineLayout_ = VK_NULL_HANDLE;  // frustum-only layout (set 0)
    VkPipelineLayout cullHiZPipelineLayout_ = VK_NULL_HANDLE; // HiZ layout (set 0 + set 1)
    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool cullDescPool_ = VK_NULL_HANDLE;
    VkDescriptorSet cullSet_[2] = {};               // double-buffered
    ::VkBuffer cullUniformBuffer_[2] = {};           // frustum planes + camera + HiZ params (UBO)
    VmaAllocation cullUniformAlloc_[2] = {};
    void* cullUniformMapped_[2] = {};
    ::VkBuffer cullInputBuffer_[2] = {};             // per-instance bounding sphere + flags (SSBO)
    VmaAllocation cullInputAlloc_[2] = {};
    void* cullInputMapped_[2] = {};
    ::VkBuffer cullOutputBuffer_[2] = {};            // uint visibility[] (SSBO, host-readable)
    VmaAllocation cullOutputAlloc_[2] = {};
    void* cullOutputMapped_[2] = {};

    // HiZ occlusion culling (Phase 6.3) - optional, driven by Renderer
    HiZSystem* hizSystem_ = nullptr;
    const std::vector<LocalLight>* frameLights_ = nullptr;

    RtScene* rtScene_ = nullptr;
    // RtScene instances this renderer owns, and per RtScene instance id the
    // sync generation that last saw it and the mesh it places.
    std::vector<uint32_t> rtOwned_;
    std::vector<uint64_t> rtSeen_;
    std::vector<uint32_t> rtMeshOf_;
    uint64_t rtSyncGeneration_ = 0;
    void registerRtModel(M2ModelGPU& gpuModel, const pipeline::M2Model& model);
    void releaseRtModel(M2ModelGPU& gpuModel);

    // Previous frame's view-projection for temporal reprojection in HiZ culling.
    // Stored each frame so the cull shader can project into the same screen space
    // as the depth buffer the HiZ pyramid was built from.
    glm::mat4 prevVP_{1.0f};

    // Instance IDs, in dispatch order, for the cull results a frame slot holds.
    //
    // The visibility SSBO is read back one full slot cycle after it is written:
    // dispatchCullCompute() records a dispatch into this frame's command buffer,
    // and render() reads the mapped output of the dispatch recorded on the SAME
    // slot ~2 frames earlier.  Array indices are not stable across that gap -
    // createInstance() appends and removeInstance() swap-removes, so slot i can
    // easily describe a different object by the time it is read.  Recording the
    // IDs lets render() match each result back to the instance it was computed
    // for.  cullSubmittedIds_ is the dispatch just recorded (results not ready);
    // cullReadableIds_ is the previous one, matching the mapped data now.

    // Dynamic ribbon vertex buffer (CPU-written triangle strip)
    // 9 floats each, for every trail in view. One riding phoenix lays 660
    // edges across its ten trails - 1300 vertices - when they run at the
    // rate and lifetime it asks for.
    static constexpr size_t MAX_RIBBON_VERTS = 65536;

    /// A vertex buffer the CPU fills every frame, one per frame in flight.
    /// There was one of each: while the GPU still drew the last frame's
    /// particles, this frame's were written over them, so particles showed a
    /// frame ahead of where they were, flashed white or vanished for a frame
    /// whenever the two overlapped.
    struct FrameVertexBuffer {
        std::array<::VkBuffer, MAX_FRAMES_IN_FLIGHT> buffer{};
        std::array<VmaAllocation, MAX_FRAMES_IN_FLIGHT> alloc{};
        std::array<void*, MAX_FRAMES_IN_FLIGHT> mapped{};
    };
    bool createFrameVertexBuffer(FrameVertexBuffer& vb, VkDeviceSize size, const char* what);
    void destroyFrameVertexBuffer(FrameVertexBuffer& vb);
    /// This frame's copy: its buffer and where it is mapped.
    ::VkBuffer frameBuffer(const FrameVertexBuffer& vb) const;
    void* frameMapped(const FrameVertexBuffer& vb) const;

    FrameVertexBuffer ribbonVB_;
    FrameVertexBuffer m2ParticleVB_;
    static constexpr size_t MAX_GLOW_SPRITES = 8000;
    FrameVertexBuffer glowVB_;

    std::unordered_map<uint32_t, M2ModelGPU> models;
    // Grace period for model cleanup: track when a model first became instanceless.
    // Models are only evicted after 60 seconds with no instances.
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> modelUnusedSince_;
    std::unordered_set<uint32_t> pinnedModelIds_;
    // Model IDs evicted by cleanupUnusedModels() since the last drain. Owners that
    // cache "already uploaded" model IDs (e.g. TerrainManager) reconcile against
    // this so a reaped model is re-loaded instead of skipped as a stale hit.
    std::vector<uint32_t> reapedModelIds_;

    // Emitters posed by another renderer; see loadPosedEmitterModel. Their
    // models hold emitters only, and the instances only the emitter state,
    // bones and transform that emitParticles and updateRibbons read.
    struct PosedEmitterModel {
        M2ModelGPU gpu;
        /// The bones the emitters hang from, so a pose copies those alone.
        std::vector<uint32_t> bones;
        uint32_t boneCount = 0;
    };
    struct PosedEmitters {
        uint32_t modelHandle = 0;
        M2Instance inst;
        bool posed = false;
    };
    std::unordered_map<uint32_t, PosedEmitterModel> posedEmitterModels_;
    uint32_t nextPosedEmitterModel_ = 1;
    uint32_t nextPosedEmitters_ = 1;
    /// Bumped whenever materialDescPool_ is reset or destroyed. A set freed
    /// after the frames in flight are done checks it first: the reset has
    /// returned it already. Shared, since the free can run after this
    /// renderer is gone.
    std::shared_ptr<uint32_t> materialPoolGeneration_ = std::make_shared<uint32_t>(0);
    void destroyPosedEmitterModels();

    // O(1) dedup: key = (modelId, quantized x, quantized y, quantized z) → instanceId
    struct DedupKey {
        uint32_t modelId;
        int32_t qx, qy, qz; // position quantized to 0.1 units
        bool operator==(const DedupKey& o) const {
            return modelId == o.modelId && qx == o.qx && qy == o.qy && qz == o.qz;
        }
    };
    struct DedupHash {
        size_t operator()(const DedupKey& k) const {
            size_t h = std::hash<uint32_t>()(k.modelId);
            h ^= std::hash<int32_t>()(k.qx) * 2654435761u;
            h ^= std::hash<int32_t>()(k.qy) * 40503u;
            h ^= std::hash<int32_t>()(k.qz) * 12289u;
            return h;
        }
    };

    uint32_t nextInstanceId = 1;
    uint32_t lastDrawCallCount = 0;
    size_t modelCacheLimit_ = 6000;
    uint32_t modelLimitRejectWarnings_ = 0;

    /// A model texture from the shared cache, loading it if it is not there;
    /// white when it cannot be had. The image is shared with every other
    /// renderer, so how a batch wraps or biases it is the batch's sampler's
    /// business (batchImageInfo), not the texture's.
    TextureRef loadTexture(const std::string& path);
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;

    /// When a key that would not load, or found no room, is next worth a read.
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint64_t textureLookupSerial_ = 0;
    TextureRef whiteTexture_;
    TextureRef glowTexture_;
    VkDescriptorSet glowTexDescSet_ = VK_NULL_HANDLE;  // cached glow texture descriptor (allocated once)

    // Optional query-space culling for collision/raycast hot paths.
    CollisionFocus collisionFocus;

    void rebuildSpatialIndex();
    /// The animated, particle and light index lists, from the
    /// instances as they now stand - after a removal has moved some of them.
    void rebuildInstanceIndexLists();
    void gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<size_t>& outIndices) const;


    /// Instances by 64-yard column, for the view and shadow passes to reject
    /// whole regions before looking at any instance in them. See M2CullGrid.
    /// Files (or refiles) an instance in cullGrid_ from its current position
    /// and cull data. Called wherever those change.
    void fileInCullGrid(const M2Instance& instance);
    /// Indices of the instances in the columns this camera may see within
    /// maxPossibleDistSq. Every instance the per-instance view test would keep
    /// is among them.
    void gatherViewCandidates(const glm::vec3& camPos, const Frustum& frustum,
                              float maxPossibleDistSq, std::vector<uint32_t>& out) const;
    /// Indices of the instances in the columns that may cast into this light
    /// box. Every instance the per-instance shadow test would keep is among
    /// them, whatever the camera is looking at.
    void gatherShadowCandidates(const glm::mat4& lightSpace, float shadowRadius,
                                std::vector<uint32_t>& out) const;
    std::vector<uint32_t> viewCandidates_;    // reused by render()
    std::vector<uint32_t> cullCandidates_;    // reused by dispatchCullCompute()
    std::vector<uint32_t> shadowCandidates_;  // reused by renderShadow()
    uint32_t cullVerdictSerial_ = 0;
    // Instance to copy bone matrices from when spawning another of the same
    // model. Finding one by scanning every instance is O(n) per spawn, which
    // became 18ms for a single createInstance once a zone held tens of
    // thousands of them.
    // Collision scratch buffers are thread_local (see m2_renderer.cpp) for thread-safety.

    // Collision query profiling - atomic because getFloorHeight is dispatched
    // on async threads from camera_controller while the main thread reads these.
    mutable std::atomic<double> queryTimeMs{0.0};
    mutable std::atomic<uint32_t> queryCallCount{0};

    // Persistent render buffers (avoid per-frame allocation/deallocation)
    struct VisibleEntry {
        uint32_t index;
        uint32_t modelId;
        float distSq;
        float effectiveMaxDistSq;
    };
    std::vector<VisibleEntry> sortedVisible_;  // Reused each frame
    std::vector<VisibleEntry> transparentVisible_; // Only models needing pass 2
    struct GlowSprite {
        glm::vec3 worldPos;
        glm::vec4 color;
        float size;
    };
    std::vector<GlowSprite> glowSprites_;  // Reused each frame

    // Shadow-pass texture descriptor cache (reused each frame, cleared via pool reset)
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    /// The casters this frame, grouped by model: [0] solid, [1] foliage.
    ///
    /// The shadow pass used to walk every instance in the world twice - once
    /// per pass - and draw them in whatever order they happened to sit in,
    /// rebinding the vertex and index buffers whenever consecutive instances
    /// came from different models, and rebinding a batch's texture once per
    /// instance rather than once per batch. Culling once into these and
    /// sorting them by model turns both into once per model.
    ///
    /// Kept as members so a frame's worth of casters costs no allocation.
    std::vector<std::pair<uint32_t, uint32_t>> shadowCasters_[2];
    /// Set by the sun's pass once it has culled into shadowCasters_, and
    /// consumed by the contact pass that follows it in the same frame.
    bool solarCastersFresh_ = false;

    // Ribbon draw-call list (reused each frame)
    struct RibbonDrawCall {
        VkDescriptorSet texSet;
        VkPipeline      pipeline;
        uint32_t        firstVertex;
        uint32_t        vertexCount;
    };
    std::vector<RibbonDrawCall> ribbonDraws_;

    // Particle group structures (reused each frame)
    struct ParticleGroupKey {
        VkTexture* texture;
        uint8_t blendType;
        uint16_t tilesX;
        uint16_t tilesY;
        bool fogged;
        bool operator==(const ParticleGroupKey& other) const {
            return texture == other.texture &&
                   blendType == other.blendType &&
                   fogged == other.fogged &&
                   tilesX == other.tilesX &&
                   tilesY == other.tilesY;
        }
    };
    struct ParticleGroupKeyHash {
        size_t operator()(const ParticleGroupKey& key) const {
            size_t h1 = std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(key.texture));
            size_t h2 = std::hash<uint32_t>{}((static_cast<uint32_t>(key.tilesX) << 16) | key.tilesY);
            size_t h3 = std::hash<uint32_t>{}(key.blendType | (key.fogged ? 0x100u : 0u));
            return h1 ^ (h2 * 0x9e3779b9u) ^ (h3 * 0x85ebca6bu);
        }
    };
    struct ParticleGroup {
        VkTexture* texture;
        uint8_t blendType;
        uint16_t tilesX;
        uint16_t tilesY;
        bool fogged = true;
        VkDescriptorSet preAllocSet = VK_NULL_HANDLE;
    };
    std::unordered_map<ParticleGroupKey, ParticleGroup, ParticleGroupKeyHash> particleGroups_;

    /// One run of particles written to the vertex buffer that share a group.
    ///
    /// The vertices used to be accumulated into a std::vector<float> per
    /// group - nine push_backs a particle - and then memcpy'd into the mapped
    /// buffer. They are written straight into it now, in the order they are
    /// walked, and a run is closed whenever the group changes. Particles from
    /// one emitter cluster together, so a run is roughly an emitter.
    ///
    /// That also fixes the upload: every group used to memcpy to offset zero
    /// and draw from vertex zero, so with more than one group on screen they
    /// all drew whichever group was copied last.
    struct ParticleRun {
        ParticleGroup* group = nullptr;
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::vector<ParticleRun> particleRuns_;
    /// Quads the particle buffer holds: a whole frame's, a particle that is
    /// both head and tail taking two. The client has no frame budget at all -
    /// each emitter draws at most m2EmitterDrawCap - so this is sized for the
    /// busiest place in view rather than as a limit; filling it is reported.
    /// 32768 filled in Dalaran before particles were culled to the view.
    static constexpr size_t MAX_M2_PARTICLE_QUADS = 131072;

    // Animation update buffers (avoid per-frame allocation)

    // Fast-path instance index lists (rebuilt in rebuildSpatialIndex / on create)

    // M2 particle emitter system. Each emitter holds as many particles as its
    // pool allows (m2EmitterPool), with no cap per instance.
    std::mt19937 particleRng_{123};
    /// Particles drawn per emitter of the instance being written; scratch.
    std::vector<uint32_t> drawnPerEmitter_;
    bool skyMode_ = false;
    float skyTimeOfDayHours_ = 12.0f;
    std::function<std::optional<float>(float, float, float)> waterHeightAt_;
    // What the sky-model clock diagnostic last reported, so it prints on a
    // restart or once a second rather than every frame. See M2Renderer::update.
    uint32_t skyDiagInstanceId_ = 0;
    float skyDiagAnimTime_ = 0.0f;
    /// Whether the sky model was drawn last frame, so the cull
    /// diagnostic prints on a change rather than every frame.
    bool skyDiagWasDrawn_ = false;
    /// Sky batches that reached a draw call this frame, and the last counts
    /// reported, so the line prints on a change rather than every frame.
    uint32_t skyDiagDrawsOpaque_ = 0;
    uint32_t skyDiagDrawsTransparent_ = 0;
    uint32_t skyDiagLastOpaque_ = 0xFFFFFFFFu;
    uint32_t skyDiagLastTransparent_ = 0xFFFFFFFFu;

    // Cached camera state from update() for frustum-culling bones
    glm::vec3 cachedCamPos_ = glm::vec3(0.0f);
    glm::mat4 cachedView_ = glm::mat4(1.0f);
    /// The camera update() last saw, for the particle draw to cull by: only
    /// what is in view goes into the particle buffer.
    glm::mat4 cachedViewProjection_{1.0f};
    float cachedMaxRenderDistSq_ = 0.0f;
    float smoothedRenderDist_ = 1000.0f;  // Smoothed render distance to prevent flickering
    /// The distance as asked for, kept so the scale can be rebuilt when the
    /// detail setting changes without the caller having to say it again.
    float viewDistanceRaw_ = 1200.0f;
    float environmentDetail_ = 1.0f;
    float groundDetailMaxDistance_ = 0.0f;   // 0 = no cap of its own
    void recomputeViewDistanceScale() {
        viewDistanceScale_ = (viewDistanceRaw_ / 1200.0f) * environmentDetail_;
    }
    float viewDistanceScale_ = 1.0f;
    float viewDistanceAbsolute_ = 1200.0f;
    /// Drop the sky model's baked star layer, because something else is
    /// drawing stars. See BatchGPU::starLayer.
    bool suppressBakedStars_ = false;
    float particleDensity_ = 1.0f;
    float furthestDrawnSq_ = 0.0f;
    bool forceNoCull_ = false;

    // Thread count for parallel bone animation
    uint32_t numAnimThreads_ = 1;

    float interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                      float globalTime, int seqIdx,
                      const std::vector<uint32_t>& globalSeqDurations);
    float interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio);
    glm::vec2 interpFBlockVec2(const pipeline::M2FBlock& fb, float lifeRatio);
    glm::vec3 interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio);
    float interpFBlockCell(const pipeline::M2FBlock& fb, float lifeRatio, uint32_t cellCount);
    void emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt);
    /// A one-shot ended: start the animation queued after it.
    void advanceToNextAnimation(M2Instance& inst);
    /// Effects ended with finishInstance, removed once nothing they made is
    /// left, or at the deadline their longest particle life sets.
    struct FinishingInstance {
        uint32_t instanceId = 0;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<FinishingInstance> finishingInstances_;
    void reapFinishedInstances();
    void updateParticles(M2Instance& inst, float dt);
    /// Ages the instance's ribbon edges and, when `emit`, lays new ones.
    void updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt, bool emit = true);
    /// The emitter half of loadModel: per-emitter textures, blend modes and
    /// descriptor sets, from the model's textures as loaded.
    void setupEmitterResources(M2ModelGPU& gpuModel, const pipeline::M2Model& model,
                               const std::vector<VkTexture*>& allTextures);

    // Helper to allocate descriptor sets
    VkDescriptorSet allocateMaterialSet();
    VkDescriptorSet allocateBoneSet();

    // Helper to destroy model GPU resources
    void destroyModelGPU(M2ModelGPU& model);
    // Helper to destroy instance bone buffers.
    // When defer=true, destruction is scheduled via deferAfterFrameFence so
    // in-flight command buffers are not invalidated (use for streaming unload).
    /// Starts a new instance's animation and seeds its bones from a
    /// sibling of the same model, so it draws on the frame it spawns.
    /// Both spawn paths need it and each used to have its own copy.
    void seedInstanceAnimation(const M2ModelGPU& model, uint32_t modelId,
                               M2Instance& instance);
    void seedUnanimatedSequence(const M2ModelGPU& model, M2Instance& instance);

    void destroyInstanceBones(M2Instance& inst, bool defer = true);
    struct InstanceState {
        std::vector<M2Instance> instances;
        std::unordered_map<uint32_t, PosedEmitters> posedEmitters_;
        std::unordered_map<DedupKey, uint32_t, DedupHash> instanceDedupMap_;
        SpatialGrid spatialGrid;
        std::unordered_map<uint32_t, size_t> instanceIndexById;
        M2CullGrid cullGrid_;
        std::unordered_map<uint32_t, uint32_t> boneSeedInstanceByModel_;
        std::vector<uint32_t> cullSubmittedIds_[2];
        std::vector<uint32_t> cullReadableIds_[2];
        std::vector<size_t> animatedInstanceIndices_;   // hasAnimation && !disableAnimation
        std::vector<size_t> particleOnlyInstanceIndices_; // !hasAnimation && hasParticleEmitters
        std::vector<size_t> particleInstanceIndices_;    // ALL instances with particle emitters
        std::vector<size_t> lightInstanceIndices_;       // models that can throw a local light
        std::vector<size_t> boneWorkIndices_;        // Reused each frame
        std::vector<std::future<void>> animFutures_; // Reused each frame
        bool spatialIndexDirty_ = false;
    };
    InstanceState defaultInstances_;
    InstanceState* scene_ = &defaultInstances_;

};

} // namespace rendering
} // namespace wowee
