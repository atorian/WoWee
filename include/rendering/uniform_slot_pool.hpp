#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wowee {
namespace rendering {

/// Small uniform buffers that live as long as what they describe - a WMO
/// material, an M2 batch's material, a terrain chunk's parameters - kept as
/// slots of a few large mapped buffers rather than a buffer each.
///
/// Each was its own VMA allocation. Host-visible memory the GPU reads at full
/// speed is the 256 MB BAR heap, and the world fills it; once it is full VMA
/// gives every new buffer a vkAllocateMemory of its own - 0.3 ms each on this
/// driver, 100-300 ms for one city WMO's materials on the main thread, and
/// 1,800 device allocations towards the driver's limit of 4,096 after a few
/// minutes of flying. A slot is an offset into a block made once.
///
/// Every slot is the same size, rounded up to the device's uniform offset
/// alignment, so a slot is a descriptor's buffer, offset and range as it is.
/// Thread-safe: slots are freed from deferred deletions as well as taken on
/// the thread that loads.
class UniformSlotPool {
public:
    /// One block's buffer, as whoever makes blocks hands it back.
    struct Block {
        ::VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    /// Makes a mapped uniform buffer of this many bytes, or none.
    using MakeBlock = std::function<std::optional<Block>(VkDeviceSize bytes)>;
    using FreeBlock = std::function<void(const Block&)>;

    struct Slot {
        ::VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        /// Where the slot's bytes are written; host-coherent, as the
        /// allocations it replaced were.
        void* mapped = nullptr;
        uint32_t id = UINT32_MAX;
        explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
    };

    /// `slotBytes` is what one holds (sizeof the UBO); it is rounded up to
    /// `alignment`. `name` says whose slots these are in the log.
    UniformSlotPool(VkDeviceSize slotBytes, VkDeviceSize alignment, uint32_t slotsPerBlock,
                    std::string name, MakeBlock make, FreeBlock free);
    /// Slots from VMA-made, persistently mapped CPU_TO_GPU buffers, counted
    /// under the uniform use like any other. Shared, so a deferred deletion
    /// can hold the pool its slots go back to.
    static std::shared_ptr<UniformSlotPool> forDevice(VmaAllocator allocator,
                                                      VkPhysicalDevice physicalDevice,
                                                      VkDeviceSize slotBytes, uint32_t slotsPerBlock,
                                                      std::string name);
    ~UniformSlotPool();

    UniformSlotPool(const UniformSlotPool&) = delete;
    UniformSlotPool& operator=(const UniformSlotPool&) = delete;

    /// A slot, from a freed one first, else a new block when every block is
    /// full. Empty when a block could not be made; that is logged once per
    /// pool and the caller draws without what the slot would have held.
    [[nodiscard]] Slot allocate();
    /// Gives a slot back. The caller frees only once no frame in flight reads
    /// it - from a deferred deletion, as with the buffers this replaces.
    /// A slot of a released pool, or an empty one, is ignored.
    void free(const Slot& slot);
    /// Lets every block go: the renderer is shutting down and the device is
    /// idle. Slots still out read nothing afterwards and free() ignores them.
    void release();

    [[nodiscard]] VkDeviceSize slotBytes() const { return slotBytes_; }
    [[nodiscard]] size_t blockCount() const;
    [[nodiscard]] size_t slotsInUse() const;

private:
    VkDeviceSize slotBytes_;
    uint32_t slotsPerBlock_;
    std::string name_;
    MakeBlock make_;
    FreeBlock freeBlock_;

    mutable std::mutex mutex_;
    std::vector<Block> blocks_;
    std::vector<uint32_t> freeSlots_;
    size_t inUse_ = 0;
    bool released_ = false;
    bool failureSaid_ = false;
};

} // namespace rendering
} // namespace wowee
