#pragma once

// The characters' draws, made on the GPU (char_draws.comp.glsl).
//
// An instance is drawn from its draw list, which the main thread builds
// when the instance's dress, model or textures change: an entry for each
// batch it can draw, with the batch's material as a row of the material
// table and the places its draws go. A frame's work on the CPU is a record
// an instance - its pose, light, tint, opacity and clock - and the cull on
// the GPU turns the entries into draws: it samples each batch's animated
// alpha and texture animation at its instance's clock, picks the place the
// instance's state asks for and writes the draw there, or leaves the places
// empty.
//
// A place is in a bucket: a part (the solid instances or the faded ones), a
// phase, whether it is the partly-faded place of a solid batch, a pipeline,
// a depth bias layer and a geometry page. A phase counts how often the
// state changed before a batch in its instance's drawing order, so the
// instance's batches keep that order across the buckets; within a bucket the
// places keep the order they were handed out in, frame after frame, and the
// bucket is drawn whole by one indirect call.
//
// CharacterDrawLayout keeps the lists, the rows and the places on the main
// thread and carries what changed in each extracted frame (CharDrawFrame);
// CharacterDrawPass takes a frame to the GPU, culls it and holds what the
// draws read.

#include "rendering/gpu_row_store.hpp"
#include "rendering/gpu_slot_table.hpp"
#include "rendering/range_allocator.hpp"
#include "rendering/vk_utils.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

/// The pipelines a character draw goes out on (CharacterRenderer::PipelineSet).
enum class CharPipeline : uint8_t {
    Opaque, AlphaTest, Alpha, Translucent, Additive, Modulate, Modulate2x, FadeDepth, FadeColor
};

/// The shadow maps a frame can cull characters into: the sun's, the
/// units', the lightning's.
inline constexpr uint32_t kCharShadowMaps = 4;

/// A row of the material table (CharMaterial in character_material.glsli).
struct CharMaterialGPU {
    uint32_t texture = 0;
    uint32_t normalHeight = 0;
    uint32_t secondTexture = 0;
    int32_t alphaTest = 0;
    int32_t unlit = 0;
    float specularIntensity = 0.0f;
    int32_t enableNormalMap = 0;
    int32_t enablePOM = 0;
    float pomScale = 0.0f;
    int32_t pomMaxSamples = 0;
    float heightMapVariance = 0.0f;
    float normalMapStrength = 0.0f;
    int32_t hairMaterial = 0;
    int32_t ignoreTextureAlpha = 0;
    float modulateNeutral = 0.0f;
    int32_t texCoordSet = 0;
    int32_t twoSided = 0;
    int32_t hasSecondTexture = 0;
    int32_t texCoordSet2 = 0;
    int32_t combiner = 0;
    glm::vec4 batchColor{1.0f};
};
static_assert(sizeof(CharMaterialGPU) == 96, "character_material.glsli's CharMaterial");

/// A batch of an instance's draw list (Entry in char_draws.comp).
struct CharEntryGPU {
    static constexpr uint32_t kNone = ~0u;
    static constexpr uint32_t kValid = 1u;
    static constexpr uint32_t kFirst = 2u;
    static constexpr uint32_t kCasterSample = 4u;
    static constexpr uint32_t kCasterAlphaTest = 8u;
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    int32_t baseVertex = 0;
    uint32_t slot = 0;
    uint32_t material = kNone;
    uint32_t colorAlpha = kNone;
    uint32_t weight = kNone;
    uint32_t flags = 0;
    glm::uvec2 uvTransforms{kNone, kNone};
    uint32_t casterTexture = 0;
    uint32_t pad = 0;
    /// (bucket, offset) of each place; bucket kNone for none.
    glm::uvec2 solid{kNone, 0u};
    glm::uvec2 partial{kNone, 0u};
    glm::uvec2 fadedDepth{kNone, 0u};
    glm::uvec2 faded{kNone, 0u};
    glm::uvec2 caster{kNone, 0u};
    glm::uvec2 pad2{0u};
};
static_assert(sizeof(CharEntryGPU) == 96, "char_draws.comp's Entry");

/// An instance this frame (CharInstance in character_material.glsli).
struct CharInstanceGPU {
    static constexpr uint32_t kPlaced = 1u;
    static constexpr uint32_t kCulled = 2u;
    static constexpr uint32_t kSeen = 4u;
    glm::uvec4 pose{0u};
    glm::vec4 interiorAmbient{0.0f};
    glm::vec4 interiorDirect{0.0f};
    glm::vec4 stateTint{1.0f};
    glm::vec4 sphere{0.0f};
    glm::vec4 clock{0.0f};
    glm::uvec4 anim{~0u, 0u, 0u, 0u};
    glm::uvec4 pad{0u};
};
static_assert(sizeof(CharInstanceGPU) == 128, "character_material.glsli's CharInstance");

/// A draw this frame (CharDraw in character_material.glsli), written by the cull.
struct CharDrawGPU {
    glm::uvec4 refs{0u};
    glm::vec4 uvRow0{0.0f}, uvRow1{0.0f}, uv2Row0{0.0f}, uv2Row1{0.0f};
    glm::vec4 opacity{0.0f};
};
static_assert(sizeof(CharDrawGPU) == 96, "character_material.glsli's CharDraw");

/// What the cull reads of a frame (Frame in char_draws.comp, std140).
struct CharCullFrameGPU {
    glm::vec4 viewPlanes[6] = {};
    /// xyz the eye, w the render radius squared.
    glm::vec4 camera{0.0f};
    /// x entries, y lights, z colour places, w caster places.
    glm::uvec4 counts{0u};
    /// x 1 when WOWEE_CHAR_ALPHA_GATE=0: every batch drawn at full alpha,
    /// whatever its colour and weight tracks say.
    glm::uvec4 flags{0u};
};

/// What the cull counted of a frame (Stats in char_draws.comp).
struct CharCullStatsGPU {
    uint32_t solidDraws = 0;
    uint32_t fadedDraws = 0;
    uint32_t casterDraws = 0;
    uint32_t drawnInstances = 0;
};

/// Which bucket a place is in: the part, the phase, whether it is a solid
/// batch's partly-faded place, the pipeline, the depth bias layer.
struct CharBucketKey {
    uint8_t part = 0;  ///< 0 solid, 1 faded
    uint16_t phase = 0;
    uint8_t partial = 0;
    CharPipeline pipeline = CharPipeline::Opaque;
    uint16_t layer = 0;
    int32_t page = 0;
    auto operator<=>(const CharBucketKey&) const = default;
};

/// A batch of an instance's list as its renderer describes it.
struct CharListEntry {
    CharEntryGPU entry;  ///< its ranges, tracks and caster; places and rows the layout's
    /// Its material; only for a batch drawn in colour.
    std::optional<CharMaterialGPU> material;
    std::optional<CharBucketKey> solid, partial, fadedDepth, faded;
    bool casts = false;
};

/// A geometry page's buffers, as a frame binds them.
struct CharPageBuffers {
    ::VkBuffer vertices = VK_NULL_HANDLE;
    ::VkBuffer indices = VK_NULL_HANDLE;
};

/// A bucket as a frame records it.
struct CharBucket {
    CharBucketKey key;
    uint32_t start = 0;
    uint32_t size = 0;
};

/// What one extracted frame carries of the layout.
struct CharDrawFrame {
    uint32_t entryRowCount = 0, materialRowCount = 0;
    std::vector<uint32_t> entrySlots, materialSlots;
    std::vector<CharEntryGPU> entryRows;
    std::vector<CharMaterialGPU> materialRows;
    /// This frame's instances, by slot.
    std::vector<CharInstanceGPU> instances;
    /// Each bucket's first place, the colour buckets then the casters'.
    std::vector<uint32_t> bucketStarts;
    /// The colour buckets in drawing order, and the casters' by page.
    std::vector<CharBucket> colour, casters;
    std::vector<CharPageBuffers> pages;
    uint32_t colourPlaces = 0;
    uint32_t casterPlaces = 0;
    CharCullFrameGPU cull;
    void clear() noexcept;
};

class CharacterDrawLayout {
public:
    void initialize(VkContext& context) { context_ = &context; }

    /// The slot an instance's records go by, given it when first asked.
    uint32_t slotOf(uint32_t id);
    /// The instance is gone: its list, rows and places, and its slot.
    void remove(uint32_t id);
    /// The instance's draw list from now on, on geometry page `page`.
    void setList(uint32_t id, int32_t page, std::vector<CharListEntry> entries);
    /// Every instance gone.
    void clear();
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(slotIds_.size()); }

    /// The rows `frame` carries, given every frame up to `applied` is on the
    /// GPU, and the buckets as they stand.
    void carry(uint32_t serial, uint32_t applied, CharDrawFrame& frame);

private:
    /// A bucket's places: handed out and taken back, the size the furthest
    /// one handed out reaches. Its id, its place in bucketStarts, is its own
    /// for as long as the layout is, so an entry's place stays good.
    struct Bucket {
        RangeAllocator places{kMaxPlaces};
        uint32_t size = 0;
        uint32_t id = 0;
    };
    struct Taken {
        Bucket* bucket = nullptr;
        uint32_t first = 0, count = 0;
    };
    struct List {
        uint32_t slot = 0;
        uint32_t entryBlock = 0, entryCount = 0;
        std::vector<uint32_t> materialRows;
        std::vector<Taken> places;
    };
    static constexpr uint32_t kMaxPlaces = 1u << 24;

    void release(List& list);
    std::optional<uint32_t> take(const CharBucketKey& key, uint32_t count, bool caster, List& list);

    VkContext* context_ = nullptr;
    std::unordered_map<uint32_t, List> lists_;
    std::unordered_map<uint32_t, uint32_t> slotById_;
    std::vector<uint32_t> slotIds_;
    uint32_t bucketIds_ = 0;
    std::vector<uint32_t> freeSlots_;
    std::map<CharBucketKey, Bucket> colourBuckets_;
    std::map<int32_t, Bucket> casterBuckets_;
    RangeAllocator entryBlocks_{kMaxPlaces};
    uint32_t entryRows_ = 0;
    GpuRowStore<CharEntryGPU> entries_;
    GpuRowStore<CharMaterialGPU> materials_;
};

class CharacterDrawPass {
public:
    CharacterDrawPass() = default;
    CharacterDrawPass(const CharacterDrawPass&) = delete;
    CharacterDrawPass& operator=(const CharacterDrawPass&) = delete;

    /// False, said, when the cull could not be made: no characters are drawn.
    bool initialize(VkContext& context, const char* owner);
    void destroy();

    /// Records into `cmd`, outside a render pass and before any of the
    /// frame's character draws: the rows `frame` carries, its instances, its
    /// lists emptied and the cull of its colour lists, each instance's
    /// lights picked from `lights` (none without a buffer), its animations
    /// read from the pool's `pages`. False, said, when there was no memory.
    bool prepare(VkCommandBuffer cmd, uint32_t slot, const CharDrawFrame& frame, const std::vector<::VkBuffer>& pages,
                 uint32_t pagesVersion, ::VkBuffer lights, uint32_t lightCount);
    /// The cull of a shadow map round `centre` out to `reach`, outside a
    /// render pass: the map's number, or nothing when the frame used every
    /// map (said) or was not prepared.
    std::optional<uint32_t> cullShadowMap(VkCommandBuffer cmd, uint32_t slot, const glm::vec3& centre, float reach);
    [[nodiscard]] bool ready(uint32_t slot) const { return slots_[slot].ready; }

    [[nodiscard]] ::VkBuffer commands(uint32_t slot) const { return slots_[slot].commands.buffer; }
    /// The draws' set (set 3 of the colour pipelines) and the casters' (set
    /// 2 of the shadow pipeline), by frame slot.
    [[nodiscard]] VkDescriptorSet drawSet(uint32_t slot) const { return slots_[slot].drawSet; }
    [[nodiscard]] VkDescriptorSet casterSet(uint32_t slot) const { return slots_[slot].casterSet; }
    [[nodiscard]] VkDescriptorSetLayout drawSetLayout() const { return drawSetLayout_; }
    [[nodiscard]] VkDescriptorSetLayout casterSetLayout() const { return casterSetLayout_; }
    [[nodiscard]] const CharCullStatsGPU& counted() const { return counted_; }

private:
    struct Slot {
        AllocatedBuffer instances, frame, bucketStarts, commands, draws, casterRecords, instanceLights, stats;
        VkDescriptorSet pageSet = VK_NULL_HANDLE, cullSet = VK_NULL_HANDLE, drawSet = VK_NULL_HANDLE,
                        casterSet = VK_NULL_HANDLE;
        uint32_t version = 0, setVersion = ~0u, tablesVersion = ~0u, pagesVersion = ~0u;
        ::VkBuffer lights = VK_NULL_HANDLE;
        uint32_t entries = 0, colourPlaces = 0, casterPlaces = 0, shadowMaps = 0;
        bool counting = false;
        bool ready = false;
    };
    bool ensure(Slot& slot, const CharDrawFrame& frame);
    void writeSets(Slot& slot);

    VkContext* context_ = nullptr;
    const char* owner_ = "";
    GpuSlotTable entries_{sizeof(CharEntryGPU), "character entry table"};
    GpuSlotTable materials_{sizeof(CharMaterialGPU), "character material table"};
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout pageSetLayout_ = VK_NULL_HANDLE, cullSetLayout_ = VK_NULL_HANDLE,
                          drawSetLayout_ = VK_NULL_HANDLE, casterSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    AllocatedBuffer noLights_;
    Slot slots_[2];
    CharCullStatsGPU counted_;
};

} // namespace rendering
} // namespace wowee
