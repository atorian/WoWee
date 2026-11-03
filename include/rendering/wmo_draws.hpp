#pragma once

// The buildings' draws, made on the GPU (wmo_cull.comp.glsl).
//
// What a building is drawn from lives in three tables on the GPU, each
// written a row at a time when it changes: where each instance stands (by
// its InstanceSlots slot), each placed group of an instance - its box in the
// world, its instance, its model group's index ranges and where its draws
// go - and each model group's index ranges, shared by every instance of the
// model.
//
// A placed group has a place of its own for each draw it can make, kept for
// as long as it is placed: in the bucket of its geometry page and kind of
// material for the colour lists, among its page's casters for the shadow
// maps. The cull fills a frame's lists - the view's, the reflection's and
// each shadow map's - by writing a draw at its places or leaving them empty,
// so a bucket keeps one order from frame to frame and is drawn whole by one
// indirect call, with no count from the GPU.
//
// WmoDrawLayout keeps the tables' rows and the places on the main thread and
// carries what changed in each extracted frame (WmoDrawFrame); WmoDrawPass
// takes a frame to the GPU, culls it and holds what the draws read.

#include "rendering/gpu_row_store.hpp"
#include "rendering/gpu_slot_table.hpp"
#include "rendering/range_allocator.hpp"
#include "rendering/vk_utils.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

/// A merged batch's kind, by the pipeline and faces it is drawn with: opaque,
/// alpha-tested, blended, each one-sided then two-sided. A colour bucket is a
/// geometry page and a kind.
inline constexpr uint32_t kWmoColourKinds = 6;
/// The first blended kind: kinds from here on are the blended layer.
inline constexpr uint32_t kWmoBlendedKinds = 4;
[[nodiscard]] constexpr uint32_t wmoColourKind(bool blended, bool alphaTested, bool twoSided) {
    return (blended ? 4u : alphaTested ? 2u : 0u) + (twoSided ? 1u : 0u);
}
/// The shadow maps a frame can cull buildings into: the sun's or the
/// lightning's, the sea bed's and the shelter's.
inline constexpr uint32_t kWmoShadowMaps = 6;

/// Where an instance stands (Instance in wmo_cull.comp, std430).
struct WmoInstanceGPU {
    glm::mat4 model{1.0f};
    /// x: 1 while it is in the world.
    glm::uvec4 state{0u};
};
static_assert(sizeof(WmoInstanceGPU) == 80, "wmo_cull.comp's Instance");

/// A placed group (Group in wmo_cull.comp, wmo.vert, wmo_shadow.vert).
struct WmoGroupGPU {
    static constexpr uint32_t kValid = 1u;
    static constexpr uint32_t kShell = 2u;
    static constexpr uint32_t kAntiportal = 4u;
    /// Its box in the world, padded; w how far its cloth swings past it.
    glm::vec4 worldMin{0.0f};
    glm::vec4 worldMax{0.0f};
    /// Its box's centre in its model.
    glm::vec4 localCentre{0.0f};
    /// x its instance's slot, y its first range, z its range count, w flags.
    glm::uvec4 refs{0u};
    /// Its first place in the colour buckets of kinds 0..3.
    glm::uvec4 slots0{0u};
    /// x, y kinds 4 and 5; z its first place among its page's casters; w its page.
    glm::uvec4 slots1{0u};
};
static_assert(sizeof(WmoGroupGPU) == 96, "wmo_cull.comp's Group");

/// One index range of a model group's merged batch (Range in wmo_cull.comp).
struct WmoRangeGPU {
    static constexpr uint32_t kNone = ~0u;
    uint32_t indexCount = 0;
    /// In its geometry page.
    uint32_t firstIndex = 0;
    int32_t baseVertex = 0;
    /// Its batch's material row; kNone for a batch that is not drawn.
    uint32_t material = kNone;
    /// Its kind in bits 0-2 and its place among the group's of that kind.
    uint32_t place = 0;
    /// Its place among the group's casters; kNone for one that casts nothing.
    uint32_t caster = kNone;
    /// What it casts with: its texture's TextureTable handle, and bit 0
    /// sample it, bit 1 alpha-test it.
    uint32_t texture = 0;
    uint32_t casterFlags = 0;
};
static_assert(sizeof(WmoRangeGPU) == 32, "wmo_cull.comp's Range");

/// What the cull reads of a frame (Frame in wmo_cull.comp, std140).
struct WmoCullFrameGPU {
    glm::vec4 viewPlanes[6] = {};
    glm::vec4 mirrorPlanes[6] = {};
    /// xyz the eye, w the horizontal view distance.
    glm::vec4 camera{0.0f};
    /// x the group distance, negative for none; y the mirror's height; z 1
    /// with a mirror.
    glm::vec4 limits{-1.0f, 0.0f, 0.0f, 0.0f};
    /// x group rows, y lights, z places a colour list, w places a shadow map.
    glm::uvec4 counts{0u};
    /// x geometry pages, y 1 when the portal walk ran.
    glm::uvec4 shape{0u};
};

/// What the cull counted of a frame (Stats in wmo_cull.comp).
struct WmoCullStatsGPU {
    uint32_t viewDraws = 0;
    uint32_t blendedDraws = 0;
    uint32_t reflectionDraws = 0;
    uint32_t casterDraws = 0;
    uint32_t drawnGroups = 0;
    uint32_t frustumCulled = 0;
    uint32_t distanceCulled = 0;
    uint32_t portalCulled = 0;
};

/// A model group's draws as the layout takes them from its renderer.
struct WmoGroupDraws {
    /// Its geometry page; nothing of it is drawn without one.
    int32_t page = -1;
    glm::vec3 localCentre{0.0f};
    /// How far its cloth swings past its box, in its model's units.
    float swayReach = 0.0f;
    bool shell = false;
    bool antiportal = false;
    /// Its ranges with their places (kind and ordinal, caster ordinal) set.
    std::vector<WmoRangeGPU> ranges;
    std::array<uint32_t, kWmoColourKinds> kindCounts{};
    uint32_t casterCount = 0;
};

/// A geometry page's buffers, as a frame binds them.
struct WmoPageBuffers {
    ::VkBuffer vertices = VK_NULL_HANDLE;
    ::VkBuffer indices = VK_NULL_HANDLE;
};

/// What one extracted frame carries of the layout: the rows changed since a
/// frame known to be on the GPU, the buckets as they stand, the portal walk's
/// verdict and the cull's view.
struct WmoDrawFrame {
    uint32_t instanceRowCount = 0, groupRowCount = 0, rangeRowCount = 0;
    std::vector<uint32_t> instanceSlots, groupSlots, rangeSlots;
    std::vector<WmoInstanceGPU> instanceRows;
    std::vector<WmoGroupGPU> groupRows;
    std::vector<WmoRangeGPU> rangeRows;
    /// Each bucket's first place and size: the colour buckets by page and
    /// kind, then the casters by page.
    std::vector<uint32_t> bucketStarts, bucketSizes;
    std::vector<WmoPageBuffers> pages;
    uint32_t colourPlaces = 0;
    uint32_t casterPlaces = 0;
    /// A bit a group row the portal walk left out; empty when it did not run.
    std::vector<uint32_t> portalCulled;
    WmoCullFrameGPU cull;
    void clear() noexcept;
};

class WmoDrawLayout {
public:
    /// What an instance is as the layout places it.
    struct Placement {
        uint32_t model = 0;
        glm::mat4 matrix{1.0f};
        float scale = 1.0f;
        bool shown = true;
        /// Each of its model's groups' box in the world, padded.
        std::span<const std::pair<glm::vec3, glm::vec3>> groupBounds;
    };

    /// The context rows are handed back through, after the frames in flight.
    void initialize(VkContext& context) { context_ = &context; }

    /// A model's groups' draws, from now on. A model drawn as before but
    /// with other material rows or textures keeps its places; another shape
    /// replaces them - true - and its instances are to be placed again.
    bool setModel(uint32_t model, std::vector<WmoGroupDraws> groups);
    void removeModel(uint32_t model);
    /// The instance in `slot` as it is now; its groups are placed when its
    /// model is set.
    void place(uint32_t slot, const Placement& placement);
    /// The slot's instance is gone.
    void free(uint32_t slot);
    /// Every instance gone, the models kept.
    void clearInstances();
    /// Whether the instance in `slot` is placed against the model as it is.
    [[nodiscard]] bool placedCurrent(uint32_t slot, uint32_t model) const;
    /// The group rows of the instance in `slot`, by model group; empty for
    /// one with none placed.
    [[nodiscard]] std::span<const uint32_t> groupRows(uint32_t slot) const;
    [[nodiscard]] uint32_t groupRowCount() const { return groups_.count(); }

    /// The rows `frame` carries, given every frame up to `applied` is on the
    /// GPU, and the buckets as they stand.
    void carry(uint32_t serial, uint32_t applied, WmoDrawFrame& frame);
    /// Every row and place forgotten: the release of the rows after the
    /// frames in flight is the caller's (the tables go with them).
    void reset();

private:
    struct Model {
        std::vector<WmoGroupDraws> groups;
        /// Where each group's ranges start in the range table.
        std::vector<uint32_t> rangeFirst;
        uint32_t rangeBlock = 0, rangeCount = 0;
        /// Changes when the model's shape does, and its instances are placed
        /// again.
        uint32_t serial = 0;
    };
    /// A group's row and places, with how many of each it took: a model
    /// replaced since may count otherwise.
    struct PlacedGroup {
        uint32_t row = GpuRowStore<WmoGroupGPU>::kNoRow;
        int32_t page = -1;
        std::array<uint32_t, kWmoColourKinds> colour{}, counts{};
        uint32_t caster = 0, casterCount = 0;
    };
    struct Instance {
        uint32_t model = 0;
        uint32_t modelSerial = 0;
        bool placed = false;
        std::vector<PlacedGroup> groups;
        std::vector<uint32_t> rows;
    };
    /// A bucket's places: handed out and taken back, the size the furthest
    /// one handed out reaches.
    struct Bucket {
        RangeAllocator places{kMaxPlaces};
        uint32_t size = 0;
    };
    static constexpr uint32_t kMaxPlaces = 1u << 24;

    Bucket& colourBucket(int32_t page, uint32_t kind);
    Bucket& casterBucket(int32_t page);
    std::optional<uint32_t> take(Bucket& bucket, uint32_t count);
    void unplace(Instance& instance);
    void writeGroups(uint32_t slot, Instance& instance, const Model& model, const Placement& placement);

    VkContext* context_ = nullptr;
    std::unordered_map<uint32_t, Model> models_;
    std::vector<Instance> instances_;
    std::vector<Bucket> colourBuckets_, casterBuckets_;
    RangeAllocator rangeBlocks_{kMaxPlaces};
    uint32_t rangeRows_ = 0;
    uint32_t modelSerial_ = 0;
    GpuRowStore<WmoInstanceGPU> instanceRows_;
    GpuRowStore<WmoGroupGPU> groups_;
    GpuRowStore<WmoRangeGPU> ranges_;
};

class WmoDrawPass {
public:
    WmoDrawPass() = default;
    WmoDrawPass(const WmoDrawPass&) = delete;
    WmoDrawPass& operator=(const WmoDrawPass&) = delete;

    /// False, said, when the cull could not be made: no buildings are drawn.
    bool initialize(VkContext& context);
    void destroy();

    /// Records into `cmd`, outside a render pass and before any of the
    /// frame's WMO draws: the rows `frame` carries, its lists emptied, and
    /// the cull of its colour lists, each drawn group's lights picked from
    /// `lights` (none without a buffer). False, said, when there was no
    /// memory: nothing of the frame is drawn.
    bool prepare(VkCommandBuffer cmd, uint32_t slot, const WmoDrawFrame& frame, ::VkBuffer lights,
                 uint32_t lightCount);
    /// The cull of a shadow map round `centre` out to `reach`, outside a
    /// render pass: the map's number in the frame, or nothing (said) when
    /// the frame has used every map or was not prepared.
    std::optional<uint32_t> cullShadowMap(VkCommandBuffer cmd, uint32_t slot, const glm::vec3& centre, float reach);
    /// Whether this slot's frame was prepared: its lists may be drawn.
    [[nodiscard]] bool ready(uint32_t slot) const { return slots_[slot].ready; }

    /// What the draws read, by frame slot.
    [[nodiscard]] ::VkBuffer commands(uint32_t slot) const { return slots_[slot].commands.buffer; }
    [[nodiscard]] ::VkBuffer colourRecords(uint32_t slot) const { return slots_[slot].colourRecords.buffer; }
    [[nodiscard]] ::VkBuffer casterRecords(uint32_t slot) const { return slots_[slot].casterRecords.buffer; }
    [[nodiscard]] ::VkBuffer groupLights(uint32_t slot) const { return slots_[slot].groupLights.buffer; }
    [[nodiscard]] const GpuSlotTable& instances() const { return instances_; }
    [[nodiscard]] const GpuSlotTable& groups() const { return groups_; }
    /// Changes whenever a buffer or table a draw's set names is replaced.
    [[nodiscard]] uint32_t drawVersion(uint32_t slot) const;

    /// What the cull of the frames that finished last counted.
    [[nodiscard]] const WmoCullStatsGPU& counted() const { return counted_; }

private:
    struct Slot {
        AllocatedBuffer frame, bucketStarts, portalCulled, commands, colourRecords, casterRecords, groupLights,
            stats;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t version = 0;
        uint32_t setVersion = ~0u;
        uint32_t tablesVersion = ~0u;
        ::VkBuffer lights = VK_NULL_HANDLE;
        uint32_t colourPlaces = 0, casterPlaces = 0, groupRows = 0, shadowMaps = 0;
        bool counting = false;
        bool ready = false;
    };
    bool ensure(Slot& slot, const WmoDrawFrame& frame);
    void writeSet(Slot& slot);
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
                 VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) const;

    VkContext* context_ = nullptr;
    GpuSlotTable instances_{sizeof(WmoInstanceGPU), "WMO instance table"};
    GpuSlotTable groups_{sizeof(WmoGroupGPU), "WMO group table"};
    GpuSlotTable ranges_{sizeof(WmoRangeGPU), "WMO range table"};
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    /// Bound for the frame's lights where there are none to bind.
    AllocatedBuffer noLights_;
    Slot slots_[2];
    WmoCullStatsGPU counted_;
};

} // namespace rendering
} // namespace wowee
