#include "rendering/gpu_cull.hpp"

#include "foundation/logger.hpp"
#include "rendering/vk_context.hpp"

#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {

void cullBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst,
                 VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dst;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

bool growCullBuffer(VkContext& context, AllocatedBuffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                    VmaMemoryUsage memory, const char* owner, const char* what, bool& replaced) {
    bytes = std::max<VkDeviceSize>(bytes, 256);
    if (buffer.buffer && buffer.info.size >= bytes) return true;
    // Half again what is wanted now, so a scene streaming in does not
    // replace it every frame.
    const VkDeviceSize next = std::max(bytes, buffer.info.size + buffer.info.size / 2);
    AllocatedBuffer made = createBuffer(context.getAllocator(), next, usage, memory);
    if (!made.buffer) {
        static core::LogBudget budget(8, "cull buffers");
        if (budget.take())
            LOG_ERROR(owner, ": no memory for its cull's ", what, " (", next, " bytes) - nothing of it is drawn");
        return false;
    }
    if (buffer.buffer) {
        DeferredResources retired(context);
        retired.buffer(buffer);
    }
    buffer = made;
    replaced = true;
    return true;
}

void readCullCounts(VkContext& context, const AllocatedBuffer& stats, void* out, size_t bytes) {
    if (!stats.info.pMappedData) return;
    vmaInvalidateAllocation(context.getAllocator(), stats.allocation, 0, VK_WHOLE_SIZE);
    std::memcpy(out, stats.info.pMappedData, bytes);
}

void clearCullTargets(VkCommandBuffer cmd, ::VkBuffer commands, ::VkBuffer stats) {
    cullBarrier(cmd, kCullDrawStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, commands, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, stats, 0, VK_WHOLE_SIZE, 0);
    cullBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

void finishColourCull(VkCommandBuffer cmd) {
    cullBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                kCullDrawStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

void beginShadowCull(VkCommandBuffer cmd) {
    cullBarrier(cmd, kCullDrawStages, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
}

void finishShadowCull(VkCommandBuffer cmd) {
    cullBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, kCullDrawStages,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
}

} // namespace rendering
} // namespace wowee
