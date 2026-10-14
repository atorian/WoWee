#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

/**
 * M2 Model Format (WoW Character/Creature Models)
 *
 * M2 files contain:
 * - Skeletal animated meshes
 * - Multiple texture units and materials
 * - Animation sequences
 * - Bone hierarchy
 * - Particle emitters, ribbon emitters, etc.
 *
 * Reference: https://wowdev.wiki/M2
 */

// Animation sequence data
struct M2Sequence {
    uint32_t id;                    // Animation ID
    uint32_t variationIndex;        // Sub-animation index
    uint32_t duration;              // Length in milliseconds
    float movingSpeed;              // Speed during animation
    uint32_t flags;                 // Animation flags
    int16_t frequency;              // Probability weight
    uint32_t replayMin;             // Minimum replay delay
    uint32_t replayMax;             // Maximum replay delay
    uint32_t blendTime;             // Blend time in ms
    glm::vec3 boundMin;             // Bounding box
    glm::vec3 boundMax;
    float boundRadius;              // Bounding sphere radius
    int16_t nextAnimation;          // Next animation in chain
    uint16_t aliasNext;             // Alias for next animation
};

// Animation track with per-sequence keyframe data
struct M2AnimationTrack {
    uint16_t interpolationType = 0; // 0=none, 1=linear, 2=hermite, 3=bezier
    int16_t globalSequence = -1;    // -1 if not a global sequence

    struct SequenceKeys {
        std::vector<uint32_t> timestamps;   // Milliseconds
        std::vector<glm::vec3> vec3Values;  // For translation/scale tracks
        std::vector<glm::quat> quatValues;  // For rotation tracks
        std::vector<float> floatValues;     // For float tracks (particle emitters)
    };
    std::vector<SequenceKeys> sequences;    // One per animation sequence

    [[nodiscard]] bool hasData() const { return !sequences.empty(); }
};

// Bone data for skeletal animation
struct M2Bone {
    int32_t keyBoneId;              // Bone ID (-1 = not key bone)
    uint32_t flags;                 // Bone flags
    int16_t parentBone;             // Parent bone index (-1 = root)
    uint16_t submeshId;             // Submesh ID
    glm::vec3 pivot;                // Pivot point

    M2AnimationTrack translation;   // Position keyframes per sequence
    M2AnimationTrack rotation;      // Rotation keyframes per sequence
    M2AnimationTrack scale;         // Scale keyframes per sequence
};

// Vertex with skinning data
struct M2Vertex {
    glm::vec3 position;
    uint8_t boneWeights[4];         // Bone weights (0-255)
    uint8_t boneIndices[4];         // Bone indices
    glm::vec3 normal;
    glm::vec2 texCoords[2];         // Two UV sets
};

// Texture unit
struct M2Texture {
    uint32_t type;                  // Texture type
    uint32_t flags;                 // Texture flags
    std::string filename;           // Texture filename (from FileData or embedded)
};

// Render batch (submesh)
struct M2Batch {
    uint8_t flags;
    int8_t priorityPlane;
    uint16_t shader;                // Shader ID
    uint16_t skinSectionIndex;      // Submesh index
    uint16_t colorIndex;            // Color animation index
    uint16_t materialIndex;         // Material index
    uint16_t materialLayer;         // Material layer
    uint16_t textureCount;          // Number of textures
    uint16_t textureIndex;          // First texture lookup index
    uint16_t textureUnit;           // Texture unit
    uint16_t transparencyIndex;     // Transparency animation index
    uint16_t textureAnimIndex;      // Texture animation index

    // Render data
    uint32_t indexStart;            // First index
    uint32_t indexCount;            // Number of indices
    uint32_t vertexStart;           // First vertex
    uint32_t vertexCount;           // Number of vertices

    // Geoset info (from submesh)
    uint16_t submeshId = 0;         // Submesh/geoset ID (determines body part group)
    uint16_t submeshLevel = 0;      // Submesh level (0=base, 1+=LOD/alternate mesh)
};

// Material / render flags (per-batch blend mode)
struct M2Material {
    uint16_t flags;       // Render flags (unlit, unfogged, two-sided, etc.)
    uint16_t blendMode;   // 0=Opaque, 1=AlphaKey, 2=Alpha, 3=Add, 4=Mod, 5=Mod2x, 6=BlendAdd, 7=Screen
};

// Texture transform (UV animation) data
struct M2TextureTransform {
    M2AnimationTrack translation;   // UV translation keyframes
    M2AnimationTrack rotation;      // UV rotation keyframes (quat)
    M2AnimationTrack scale;         // UV scale keyframes
};

// Attachment point (bone-anchored position for weapons, effects, etc.)
struct M2Attachment {
    uint32_t id;        // 0=Head, 1=RightHand, 2=LeftHand, etc.
    uint16_t bone;      // Bone index
    glm::vec3 position; // Offset from bone pivot
};

// Camera baked into the model. Scene models (the character-select glue screens,
// cinematics) carry the framing the artist authored them for; without it, a scene
// whose geometry sits hundreds of units from its origin cannot be placed sensibly.
struct M2Camera {
    /// 0 is the portrait camera, 1 the character-sheet one, -1 anything else.
    /// Kept because the portrait framing is chosen by it: a model that carries
    /// its own portrait camera knows where its face is far better than a
    /// fraction of the bind-pose height can guess.
    int32_t type = -1;
    float fov = 0.0f;      // radians
    float farClip = 0.0f;
    float nearClip = 0.0f;
    glm::vec3 positionBase{0.0f};
    glm::vec3 targetBase{0.0f};  // the point the camera looks at
};

// FBlock: particle lifetime curve (color/alpha/scale over particle life)
struct M2FBlock {
    std::vector<float> timestamps;      // Fractions of the particle's life
    std::vector<float> floatValues;     // For alpha and atlas cells
    std::vector<glm::vec2> vec2Values;  // For scale: half width, half height
    std::vector<glm::vec3> vec3Values;  // For color RGB
};

/// Emitter flags (M2Particle +0x04) the renderer acts on; the rest are in
/// docs/wow-client-render-spec.md, section 4.5.
constexpr uint32_t kM2EmitterUnlit = 0x1;
/// A head stretched along its projected flight.
constexpr uint32_t kM2EmitterAlongVelocity = 0x4;
constexpr uint32_t kM2EmitterUnfogged = 0x8;
/// Particles live in the emitter's space and move with it.
constexpr uint32_t kM2EmitterModelSpace = 0x10;
/// Particle size times the emitter bone's world scale.
constexpr uint32_t kM2EmitterBoneScale = 0x20;
/// Sphere emitters: particles leave along +Z, not out from the centre.
constexpr uint32_t kM2EmitterSphereUp = 0x100;
/// Spin direction random per particle.
constexpr uint32_t kM2EmitterRandomSpin = 0x200;
/// A tail is no longer than the particle is old: a newborn has none.
constexpr uint32_t kM2EmitterClampTailToAge = 0x400;
/// Heads lie flat in the XY plane (the world's, or the emitter's with 0x10)
/// instead of facing the camera.
constexpr uint32_t kM2EmitterPlane = 0x1000;
constexpr uint32_t kM2EmitterHead = 0x20000;
constexpr uint32_t kM2EmitterTail = 0x40000;
/// scaleVary's x and y vary independently.
constexpr uint32_t kM2EmitterIndependentScaleVary = 0x80000;

/// Emitter flag 0x10000: with more than one atlas cell, each particle takes a
/// random one when it spawns (WoW.exe 0x979f8e) instead of following the cell
/// tracks.
constexpr uint32_t kM2EmitterRandomCell = 0x10000;

/// Emitter flag 0x10000000 (WotLK): the emitter blends up to three textures,
/// and its texture field packs their indices five bits each.
constexpr uint32_t kM2EmitterMultiTexture = 0x10000000;

/// The texture an emitter draws with. For a multi-texture emitter that is the
/// first of the packed three; read whole, the packed value is out of range
/// and the particle was drawn white. Blending in the other two is not done.
inline uint16_t m2EmitterBaseTexture(uint32_t flags, uint16_t texture) {
    return (flags & kM2EmitterMultiTexture) ? static_cast<uint16_t>(texture & 0x1F) : texture;
}

// Particle emitter definition parsed from M2
struct M2ParticleEmitter {
    int32_t particleId;
    uint32_t flags;
    glm::vec3 position;
    uint16_t bone;
    uint16_t texture;       // index into the model's textures; see m2EmitterBaseTexture
    uint8_t blendingType;   // 0=opaque,1=alphakey,2=alpha,4=add
    uint8_t emitterType;    // 1=plane,2=sphere,3=spline
    int16_t textureTileRotation = 0;
    uint16_t textureRows = 1;
    uint16_t textureCols = 1;
    M2AnimationTrack emissionSpeed;
    M2AnimationTrack speedVariation;
    M2AnimationTrack verticalRange;
    M2AnimationTrack horizontalRange;
    M2AnimationTrack gravity;
    M2AnimationTrack lifespan;
    /// Each particle's life varies by up to this either way (WotLK 0xAC).
    float lifespanVary = 0.0f;
    M2AnimationTrack emissionRate;
    /// The rate varies by up to this either way, drawn once a frame (0xC4).
    float emissionRateVary = 0.0f;
    M2AnimationTrack emissionAreaLength;
    M2AnimationTrack emissionAreaWidth;
    /// Above zero, particles leave away from (0, 0, zSource) in the emitter's
    /// space rather than up its axis. Read as a deceleration once; it is not.
    M2AnimationTrack zSource;
    /// How fast a particle slows, per second (WotLK; zero before it).
    float drag = 0.0f;
    /// Seconds of flight a tail particle trails behind it: the quad runs from
    /// the particle back along its velocity times this.
    float tailLength = 0.0f;
    /// Whether a particle is drawn as a head (a billboard at its position), a
    /// tail (a quad stretched back along its flight), or both.
    bool drawsHead = true;
    bool drawsTail = false;
    M2FBlock particleColor;   // vec3 RGB at 3 timestamps
    M2FBlock particleAlpha;   // float (from uint16/32767) at 3 timestamps
    M2FBlock particleScale;   // half width and height (vec2Values)
    /// Each particle's size factor varies by up to this, fixed for its life.
    glm::vec2 scaleVary{0.0f};
    /// Twinkle: the particle's slot in a table of 128 random fractions moves
    /// on at this many a second; it shows only while its slot is at or below
    /// the percentage, and its size is scaled from min to max by the slot.
    float twinkleSpeed = 0.0f;
    float twinklePercent = 1.0f;
    float twinkleScaleMin = 1.0f;
    float twinkleScaleMax = 1.0f;
    /// Spin, radians: a base angle and a rate, each give or take its variation.
    float baseSpin = 0.0f;
    float baseSpinVary = 0.0f;
    float spin = 0.0f;
    float spinVary = 0.0f;
    /// Wind added to a particle's velocity, per second, while it is younger
    /// than windTime.
    glm::vec3 wind{0.0f};
    float windTime = 0.0f;
    /// The atlas cell a head and a tail show over the particle's life (0x13C,
    /// 0x14C), unless flag kM2EmitterRandomCell picks one at spawn.
    M2FBlock headCellTrack;
    M2FBlock tailCellTrack;
    bool enabled = true;
};

// Ribbon emitter definition parsed from M2 (WotLK format)
struct M2RibbonEmitter {
    int32_t  ribbonId   = 0;
    uint32_t bone       = 0;        // Bone that drives the ribbon spine
    glm::vec3 position{0.0f};       // Offset from bone pivot

    uint16_t textureIndex  = 0;     // First texture, an index into M2Model::textures
    uint16_t materialIndex = 0;     // First material, an index into M2Model::materials (blend mode)

    // Animated tracks
    M2AnimationTrack colorTrack;       // RGB 0..1
    M2AnimationTrack alphaTrack;       // float 0..1 (stored as fixed16 on disk)
    M2AnimationTrack heightAboveTrack; // Half-width above bone
    M2AnimationTrack heightBelowTrack; // Half-width below bone
    M2AnimationTrack visibilityTrack;  // 0=hidden, 1=visible

    float edgesPerSecond = 15.0f;   // How many edge points are generated per second
    float edgeLifetime   = 0.5f;    // Seconds before edges expire
    float gravity        = 0.0f;    // Downward pull on edges per s²
    uint16_t textureRows = 1;
    uint16_t textureCols = 1;
};

// Complete M2 model structure
struct M2Model {
    // Model metadata
    std::string name;
    uint32_t version;
    glm::vec3 boundMin;             // Model bounding box
    glm::vec3 boundMax;
    float boundRadius;              // Bounding sphere

    // Geometry data
    std::vector<M2Vertex> vertices;
    std::vector<uint16_t> indices;

    // Skeletal animation
    std::vector<M2Bone> bones;
    std::vector<M2Sequence> sequences;
    std::vector<uint32_t> globalSequenceDurations;  // Per-global-sequence loop durations (ms)

    // Footfall ($FSD) animation event times per sequence index, in ms from the
    // start of the sequence, sorted ascending. These are the authored keyframes
    // the game client uses to sync footstep sounds to feet hitting the ground.
    // Empty inner list = no footfall events for that sequence.
    std::vector<std::vector<uint32_t>> footstepEventTimes;

    // Bone lookup table (vertex bone indices reference this to get global bone index)
    std::vector<uint16_t> boneLookupTable;

    // Rendering
    std::vector<M2Batch> batches;
    std::vector<M2Texture> textures;
    std::vector<uint16_t> textureLookup;  // Batch texture index lookup
    std::vector<M2Material> materials;    // Render flags / blend modes

    // Texture transforms (UV animation)
    std::vector<M2TextureTransform> textureTransforms;
    std::vector<uint16_t> textureTransformLookup;
    std::vector<int16_t> textureCoordLookup;
    /// Flag 0x08 only: each batch's stage ops, from its shader id onwards
    /// (m2BatchShading in rendering/m2_combiner.hpp).
    std::vector<uint16_t> textureCombinerCombos;

    // Texture weights (per-batch opacity, from M2Track<fixed16>)
    // Each entry is the "at-rest" opacity value (0=transparent, 1=opaque).
    // batch.transparencyIndex → textureWeightLookup[idx] → textureWeights[trackIdx]
    std::vector<float> textureWeights;
    std::vector<M2AnimationTrack> textureWeightTracks;
    std::vector<uint16_t> textureWeightLookup;

    // Color animation alpha values (from M2Color.alpha M2Track<fixed16>)
    // One entry per color animation slot; batch.colorIndex indexes directly into this.
    // Value 0=transparent, 1=opaque. Independent from textureWeights.
    std::vector<float> colorAlphas;
    /// At-rest RGB of each colour track, parallel to colorAlphas.
    ///
    /// An M2Color is a vec3 colour track followed by an alpha track, and only
    /// the alpha was read. The colour is what tints a batch: Orgrimmar's
    /// bonfire glow is authored white and carries (1.0, 0.329, 0.0) here, so
    /// without it the fire renders as a white blob.
    std::vector<glm::vec3> colorRGB;

    // Full per-sequence alpha keyframes for the same color slots. Evaluated at
    // render time to hide batches whose alpha animates to 0 in the current
    // animation (e.g. the lumberjack carry model's alternate wood bundle).
    std::vector<M2AnimationTrack> colorAlphaTracks;

    // Attachment points (for weapon/effect anchoring)
    std::vector<M2Attachment> attachments;
    std::vector<M2Camera> cameras;
    std::vector<uint16_t> attachmentLookup; // attachment ID → index

    // Particle emitters
    std::vector<M2ParticleEmitter> particleEmitters;

    // Ribbon emitters
    std::vector<M2RibbonEmitter> ribbonEmitters;

    // Collision mesh (simplified geometry for physics)
    std::vector<glm::vec3> collisionVertices;
    std::vector<uint16_t> collisionIndices;      // 3 per triangle
    std::vector<glm::vec4> collisionNormals;     // xyz=normal, w=distance; one per triangle

    // Flags
    uint32_t globalFlags;

    [[nodiscard]] bool isValid() const {
        return !vertices.empty() && !indices.empty();
    }
};

class M2Loader {
public:
    /**
     * Load M2 model from raw file data
     *
     * @param m2Data Raw M2 file bytes
     * @return Parsed M2 model
     */
    static M2Model load(const std::vector<uint8_t>& m2Data);

    /**
     * Load M2 skin file (contains submesh/batch data)
     *
     * @param skinData Raw M2 skin file bytes
     * @param model Model to populate with skin data
     * @return True if successful
     */
    static bool loadSkin(const std::vector<uint8_t>& skinData, M2Model& model);

    /**
     * Load external .anim file data into model bone tracks
     *
     * @param m2Data Original M2 file bytes (contains track headers)
     * @param animData Raw .anim file bytes
     * @param sequenceIndex Which sequence index this .anim file provides data for
     * @param model Model to patch with animation data
     */
    static void loadAnimFile(const std::vector<uint8_t>& m2Data,
                             const std::vector<uint8_t>& animData,
                             uint32_t sequenceIndex,
                             M2Model& model);
};

std::string skinPathForM2(const std::string& m2Path);

/// The .m2 a model reference means, whatever it was written as.
///
/// WMO doodad lists, ADT doodad lists and GameObjectDisplayInfo all name models
/// with the extension the art was authored with - .mdx, and occasionally .mdl -
/// while what ships is .m2. Every reader has to rewrite it, and nine of them
/// did, in their own words:
///
///   * four rewrote .mdx and .mdl
///   * three rewrote only .mdx, so a .mdl reference reached the asset manager
///     as ".mdl", found nothing, and the doodad silently did not appear
///
/// Both are handled here. A path with any other extension, or none, comes back
/// unchanged - this rewrites a known alias and does not guess.
std::string modelPathToM2(const std::string& modelPath);

} // namespace pipeline
} // namespace wowee
