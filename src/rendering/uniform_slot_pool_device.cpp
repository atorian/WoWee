// The slot pool's blocks as the renderer makes them: VMA buffers, apart from
// the pool itself so its behaviour can be tested without a device.
#include "rendering/uniform_slot_pool.hpp"

#include "rendering/gpu_allocations.hpp"

namespace wowee {
namespace rendering {

std::shared_ptr<UniformSlotPool> UniformSlotPool::forDevice(VmaAllocator allocator,
                                                            VkPhysicalDevice physicalDevice,
                                                            VkDeviceSize slotBytes, uint32_t slotsPerBlock,
                                                            std::string name) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    auto make = [allocator](VkDeviceSize bytes) -> std::optional<Block> {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        Block block;
        VmaAllocationInfo info{};
        if (gpuCreateBuffer(allocator, &bci, &aci, &block.buffer, &block.allocation, &info) != VK_SUCCESS ||
            !info.pMappedData) {
            if (block.buffer) gpuDestroyBuffer(allocator, block.buffer, block.allocation);
            return std::nullopt;
        }
        block.mapped = info.pMappedData;
        return block;
    };
    auto free = [allocator](const Block& block) {
        gpuDestroyBuffer(allocator, block.buffer, block.allocation);
    };
    return std::make_shared<UniformSlotPool>(slotBytes, props.limits.minUniformBufferOffsetAlignment,
                                             slotsPerBlock, std::move(name), std::move(make),
                                             std::move(free));
}

} // namespace rendering
} // namespace wowee
