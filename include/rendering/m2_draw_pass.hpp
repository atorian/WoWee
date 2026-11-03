#pragma once

// The M2 view's draw commands, written on the GPU (m2_draws.comp.glsl).
//
// The CPU culls and groups: each visible instance's base record in a row
// with the others of its model at its level of detail. This pass turns each
// group into the draws its model makes at that level - a command for each
// batch, at a place the CPU chose for it in its bucket - and gives the
// batches that need it records of their own: their texture animation
// sampled at the instance's clock, their opacity. The CPU then records one
// indirect call a bucket.
//
// A model's draw list, in the SkeletonPool's pages as words:
//   header   per level of detail 0..3: its entries' first word, their
//            count, its own-record list's first word, its count (16 words)
//   entries  kDrawEntryWords each: index count, first index, base vertex,
//            material row, its bucket among the level's, its place in that
//            bucket, the two stages' texture transforms (~0 for none), its
//            opacity, flags (kDrawEntryOwnRecords), its place among those
//            with records of their own, one word unused
//   own      per level, the entries that need records of their own, in
//            that order

#include "rendering/vk_utils.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

class VkContext;

inline constexpr uint32_t kDrawListLevels = 4;
inline constexpr uint32_t kDrawListHeaderWords = kDrawListLevels * 4;
inline constexpr uint32_t kDrawEntryWords = 12;
inline constexpr uint32_t kDrawEntryOwnRecords = 1u;

/// One group of a frame (Group in m2_draws.comp, std430).
struct M2DrawGroupGPU {
    uint32_t listPage = 0;
    uint32_t listBlob = 0;
    uint32_t lod = 0;
    uint32_t firstRecord = 0;
    uint32_t instanceCount = 0;
    uint32_t ownFirst = 0;
    uint32_t offsetsFirst = 0;
    uint32_t uvPage = ~0u;
    uint32_t uvBlob = 0;
    uint32_t opacityFirst = ~0u;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};
static_assert(sizeof(M2DrawGroupGPU) == 48, "m2_draws.comp's Group");

/// What the CPU hands the pass for a frame.
struct M2DrawFrame {
    std::vector<M2DrawGroupGPU> groups;
    /// Each group's buckets' first commands (M2DrawGroupGPU::offsetsFirst).
    std::vector<uint32_t> offsets;
    /// Each base record's clock: animation and global time in ms, the
    /// sequence index's bits, unused.
    std::vector<glm::vec4> clocks;
    /// The sky's opacity of each of its batches this frame.
    std::vector<float> skyOpacity;
    std::vector<::VkBuffer> pages;
    uint32_t pagesVersion = 0;
    void clear() noexcept;
};

class M2DrawPass {
public:
    /// The buffers the commands and records are written into: the frame
    /// slot's instance records, draw commands and draw material rows.
    struct Targets {
        ::VkBuffer instances = VK_NULL_HANDLE;
        ::VkBuffer commands = VK_NULL_HANDLE;
        ::VkBuffer drawMaterials = VK_NULL_HANDLE;
    };

    M2DrawPass() = default;
    M2DrawPass(const M2DrawPass&) = delete;
    M2DrawPass& operator=(const M2DrawPass&) = delete;

    /// False, said, when the pass could not be made: no M2 view draws.
    bool initialize(VkContext& context, const char* owner);
    void destroy();

    /// Writes `frame`'s commands and records into `targets`, outside a render
    /// pass, and makes them visible to the draws. False, said, when there
    /// was no memory: none of the frame's view draws are written.
    bool record(VkCommandBuffer cmd, uint32_t slot, const M2DrawFrame& frame, const Targets& targets);

private:
    struct Slot {
        AllocatedBuffer groups, offsets, clocks, skyOpacity;
        uint32_t groupRoom = 0, offsetRoom = 0, clockRoom = 0, opacityRoom = 0;
        VkDescriptorSet pageSet = VK_NULL_HANDLE;
        VkDescriptorSet frameSet = VK_NULL_HANDLE;
        Targets targets;
        uint32_t pagesVersion = ~0u;
        bool frameSetCurrent = false;
    };
    bool ensure(Slot& slot, const M2DrawFrame& frame);
    void writePageSet(Slot& slot, const std::vector<::VkBuffer>& pages, uint32_t pagesVersion);
    void writeFrameSet(Slot& slot, const Targets& targets);

    VkContext* context_ = nullptr;
    const char* owner_ = "";
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout pageSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout frameSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    Slot slots_[2];
};

} // namespace rendering
} // namespace wowee
