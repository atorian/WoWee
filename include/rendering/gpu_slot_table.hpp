#pragma once

// A table on the GPU with a row per slot, in device memory and changed a row
// at a time. The rows changed since the frame before are staged in this frame
// slot's buffer and copied in at the start of the frame's commands, after
// every earlier frame's reads of the table: one queue, in order, so neither a
// copy of the table per frame in flight nor versions of a row are needed.
//
// Its owner decides which rows changed and when a change counts as on the GPU
// (the frame that carried it was submitted); this is only how they get there.
// The M2 cull and instance tables are two (stage 2), the terrain's chunks
// another.

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

#include "rendering/frames_in_flight.hpp"

namespace wowee {
namespace rendering {

class VkContext;

class GpuSlotTable {
public:
    /// Rows of `rowBytes`; `what` names it in what is logged.
    GpuSlotTable(VkDeviceSize rowBytes, const char* what) : rowBytes_(rowBytes), what_(what) {}
    ~GpuSlotTable();
    GpuSlotTable(const GpuSlotTable&) = delete;
    GpuSlotTable& operator=(const GpuSlotTable&) = delete;

    void initialize(VkContext& context) { context_ = &context; }
    /// Frees the table and its staging now; the device must be idle.
    void destroy();

    /// Room for `slots` rows. A table that grows is replaced by one whose old
    /// rows are copied over on the GPU and whose new rows are zero. False,
    /// logged, when there is no memory: the table stays as it was.
    bool reserve(VkCommandBuffer cmd, uint32_t slots);
    /// `rows` - one of rowBytes for each of `slots` - into their slots,
    /// staged through frame slot `frameIndex`'s buffer. Every shader stage
    /// that runs after it in the queue reads them. False, logged, when there
    /// was no memory for the staging: nothing is written.
    bool write(VkCommandBuffer cmd, uint32_t frameIndex, std::span<const uint32_t> slots, const void* rows);
    /// Every row zero, after every earlier read and before every later one.
    void clear(VkCommandBuffer cmd);

    [[nodiscard]] VkBuffer buffer() const { return buffer_; }
    [[nodiscard]] uint32_t capacity() const { return capacity_; }
    [[nodiscard]] VkDeviceSize bytes() const { return VkDeviceSize(capacity_) * rowBytes_; }
    /// Changes whenever buffer() is replaced, so a set naming it is
    /// rewritten; never 0 once there is a buffer.
    [[nodiscard]] uint32_t version() const { return version_; }

private:
    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void* mapped = nullptr;
        uint32_t rows = 0;
    };

    VkContext* context_ = nullptr;
    VkDeviceSize rowBytes_;
    const char* what_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = VK_NULL_HANDLE;
    uint32_t capacity_ = 0;
    uint32_t version_ = 0;
    Staging staging_[MAX_FRAMES_IN_FLIGHT];
    std::vector<VkBufferCopy> regions_;
};

} // namespace rendering
} // namespace wowee
