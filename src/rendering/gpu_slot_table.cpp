#include "rendering/gpu_slot_table.hpp"

#include "core/logger.hpp"
#include "rendering/gpu_allocations.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_utils.hpp"

#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {

// What reads a table: the vertex, fragment and compute shaders, and the
// indirect commands a compute shader writes from it.
constexpr VkPipelineStageFlags2 kReaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;

uint32_t grownCapacity(uint32_t capacity, uint32_t need) { return std::max({need, capacity * 2u, 1024u}); }

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

// Every earlier read and copy of the table done before this copy writes it.
void beforeWrite(VkCommandBuffer cmd) {
    barrier(cmd, kReaders | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

// ...and every later read after it.
void afterWrite(VkCommandBuffer cmd) {
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            kReaders | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

} // namespace

GpuSlotTable::~GpuSlotTable() { destroy(); }

void GpuSlotTable::destroy() {
    if (!context_) return;
    VmaAllocator allocator = context_->getAllocator();
    rendering::destroy(allocator, buffer_, alloc_);
    for (auto& staging : staging_) {
        rendering::destroy(allocator, staging.buffer, staging.alloc);
        staging = {};
    }
    capacity_ = 0;
}

bool GpuSlotTable::reserve(VkCommandBuffer cmd, uint32_t slots) {
    if (slots <= capacity_) return true;
    const uint32_t capacity = grownCapacity(capacity_, slots);
    VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = VkDeviceSize(capacity) * rowBytes_;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkBuffer next = VK_NULL_HANDLE;
    VmaAllocation nextAlloc = VK_NULL_HANDLE;
    if (gpuCreateBuffer(context_->getAllocator(), &info, &allocInfo, &next, &nextAlloc, nullptr) != VK_SUCCESS) {
        static core::LogBudget budget(4, "GPU slot tables");
        if (budget.take())
            LOG_ERROR(what_, ": no memory for ", capacity, " rows of ", rowBytes_,
                      " bytes; what it holds past its ", capacity_, " is not drawn");
        return false;
    }
    // The rows so far come across on the GPU, after every earlier frame's
    // copies into and reads of the old table; the new rows start zero.
    const VkDeviceSize kept = bytes();
    beforeWrite(cmd);
    if (buffer_) {
        const VkBufferCopy region{.srcOffset = 0, .dstOffset = 0, .size = kept};
        vkCmdCopyBuffer(cmd, buffer_, next, 1, &region);
    }
    vkCmdFillBuffer(cmd, next, kept, info.size - kept, 0);
    afterWrite(cmd);
    DeferredResources retire(*context_);
    if (buffer_) retire.buffer(buffer_, alloc_);
    buffer_ = next;
    alloc_ = nextAlloc;
    capacity_ = capacity;
    ++version_;
    return true;
}

bool GpuSlotTable::write(VkCommandBuffer cmd, uint32_t frameIndex, std::span<const uint32_t> slots,
                         const void* rows) {
    if (slots.empty()) return true;
    const auto count = static_cast<uint32_t>(slots.size());
    Staging& staging = staging_[frameIndex];
    if (count > staging.rows) {
        const uint32_t capacity = grownCapacity(staging.rows, count);
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = VkDeviceSize(capacity) * rowBytes_;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        Staging next;
        if (gpuCreateBuffer(context_->getAllocator(), &info, &allocInfo, &next.buffer, &next.alloc, &mapped) !=
            VK_SUCCESS) {
            static core::LogBudget budget(4, "GPU slot table staging");
            if (budget.take())
                LOG_ERROR(what_, ": no memory to stage ", count, " changed rows; they are written next frame");
            return false;
        }
        next.mapped = mapped.pMappedData;
        next.rows = capacity;
        // Staging of a frame slot whose fence has passed: nothing reads it.
        DeferredResources retire(*context_);
        if (staging.buffer) retire.buffer(staging.buffer, staging.alloc);
        staging = next;
    }
    std::memcpy(staging.mapped, rows, VkDeviceSize(count) * rowBytes_);
    regions_.resize(count);
    for (uint32_t k = 0; k < count; ++k) {
        regions_[k] = VkBufferCopy{.srcOffset = VkDeviceSize(k) * rowBytes_,
                                   .dstOffset = VkDeviceSize(slots[k]) * rowBytes_, .size = rowBytes_};
    }
    beforeWrite(cmd);
    vkCmdCopyBuffer(cmd, staging.buffer, buffer_, count, regions_.data());
    afterWrite(cmd);
    return true;
}

void GpuSlotTable::clear(VkCommandBuffer cmd) {
    if (!buffer_) return;
    beforeWrite(cmd);
    vkCmdFillBuffer(cmd, buffer_, 0, VK_WHOLE_SIZE, 0);
    afterWrite(cmd);
}

} // namespace rendering
} // namespace wowee
