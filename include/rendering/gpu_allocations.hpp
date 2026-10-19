#pragma once

// Every buffer and image the renderer allocates goes through here, so the
// video memory held for each use (gpu_memory.hpp) is known at any moment.
//
// It used to be found by asking the allocator for its detailed map - every
// allocation, as JSON - and sorting that: 191,000 allocations in Dalaran,
// 150-300 ms of the main thread, once every ten seconds, which was the stall
// in every window of the frame profile. Counted here as each allocation is
// made and freed instead, it costs two atomic adds an allocation and nothing
// to read.
//
// The four calls take the same arguments as the VMA calls they replace and do
// the same thing besides counting. The use an allocation was counted under is
// kept in its VMA user data, so freeing it takes off exactly what making it
// put on. tools/gpu_allocation_check.py holds the calls made to VMA directly
// anywhere else at zero: one would leave the counts wrong without a trace.

#include "rendering/gpu_memory.hpp"

#include <optional>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace wowee {
namespace rendering {

VkResult gpuCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* bufferInfo,
                         const VmaAllocationCreateInfo* allocationInfo, VkBuffer* buffer,
                         VmaAllocation* allocation, VmaAllocationInfo* allocationOut);
VkResult gpuCreateImage(VmaAllocator allocator, const VkImageCreateInfo* imageInfo,
                        const VmaAllocationCreateInfo* allocationInfo, VkImage* image,
                        VmaAllocation* allocation, VmaAllocationInfo* allocationOut);
void gpuDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);
void gpuDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation);

/// Count what this allocator hands out from now on; the owner of an allocator
/// calls this as it creates it, and the second as it lets it go.
void beginCountingGpuMemory(VmaAllocator allocator);
void endCountingGpuMemory(VmaAllocator allocator);

/// What the allocator holds now, by use. Empty for one nobody began counting.
[[nodiscard]] std::optional<GpuMemoryByUse> countedGpuMemory(VmaAllocator allocator);

} // namespace rendering
} // namespace wowee
