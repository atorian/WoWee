#include "rendering/gpu_allocations.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace wowee {
namespace rendering {

namespace {

/// The running totals for one allocator.
struct Counted {
    /// Null once its counting has ended. Read without the registry's lock as
    /// allocations are freed on any thread.
    std::atomic<VmaAllocator> allocator{VK_NULL_HANDLE};
    /// Whether each memory type lives on a device-local heap, read once.
    std::vector<bool> deviceLocalType;
    std::array<std::atomic<uint64_t>, kGpuMemoryUseCount> deviceLocalBytes{};
    std::array<std::atomic<uint64_t>, kGpuMemoryUseCount> hostBytes{};
    std::atomic<uint64_t> allocations{0};
};

// The use rides in the low bits of the pointer to the totals it was added to.
static_assert(alignof(Counted) >= 8, "three low bits of a Counted* are free");
static_assert(kGpuMemoryUseCount < 8, "a use and one more fit in three bits");
constexpr uintptr_t kUseMask = 7;

/// Every allocator ever counted. Entries are never freed, only retired, so an
/// allocation that outlives its allocator's counting still points at memory
/// that exists; there are one or two in a process.
struct Registry {
    std::mutex mutex;
    std::vector<std::unique_ptr<Counted>> entries;
    std::vector<VmaAllocator> warnedUncounted;
};

Registry& registry() {
    static Registry r;
    return r;
}

Counted* countedFor(VmaAllocator allocator) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    for (const auto& e : r.entries) {
        if (e->allocator.load() == allocator) return e.get();
    }
    if (std::find(r.warnedUncounted.begin(), r.warnedUncounted.end(), allocator) ==
        r.warnedUncounted.end()) {
        r.warnedUncounted.push_back(allocator);
        LOG_WARNING("Video memory: an allocator nobody began counting is allocating; "
                    "what it holds is missing from the breakdown by use");
    }
    return nullptr;
}

void* tagFor(Counted* counted, GpuMemoryUse use) {
    if (!counted) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(counted) |
                                   (static_cast<uintptr_t>(use) + 1));
}

void add(Counted* counted, GpuMemoryUse use, const VmaAllocationInfo& info, bool adding) {
    if (!counted) return;
    const bool local = info.memoryType < counted->deviceLocalType.size() &&
                       counted->deviceLocalType[info.memoryType];
    auto& bytes = (local ? counted->deviceLocalBytes : counted->hostBytes)[static_cast<size_t>(use)];
    if (adding) {
        bytes.fetch_add(info.size, std::memory_order_relaxed);
        counted->allocations.fetch_add(1, std::memory_order_relaxed);
    } else {
        bytes.fetch_sub(info.size, std::memory_order_relaxed);
        counted->allocations.fetch_sub(1, std::memory_order_relaxed);
    }
}

/// Takes off what making this allocation put on, from the tag it carries.
void uncount(VmaAllocator allocator, VmaAllocation allocation) {
    if (allocator == VK_NULL_HANDLE || allocation == VK_NULL_HANDLE) return;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, allocation, &info);
    const auto tag = reinterpret_cast<uintptr_t>(info.pUserData);
    if ((tag & kUseMask) == 0) return;   // made where nothing was counting
    auto* counted = reinterpret_cast<Counted*>(tag & ~kUseMask);
    if (counted->allocator.load() != allocator) return;   // its counting has ended
    add(counted, static_cast<GpuMemoryUse>((tag & kUseMask) - 1), info, false);
}

/// Make the allocation with its use in its user data, and count it if it was
/// made.
template <class Create>
VkResult createCounted(VmaAllocator allocator, const VmaAllocationCreateInfo* allocationInfo,
                       GpuMemoryUse use, VmaAllocationInfo* allocationOut, Create&& create) {
    Counted* counted = countedFor(allocator);
    VmaAllocationCreateInfo tagged = *allocationInfo;
    tagged.pUserData = tagFor(counted, use);
    VmaAllocationInfo info{};
    const VkResult r = create(&tagged, &info);
    if (r == VK_SUCCESS) add(counted, use, info, true);
    if (allocationOut) *allocationOut = info;
    return r;
}

} // namespace

VkResult gpuCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* bufferInfo,
                         const VmaAllocationCreateInfo* allocationInfo, VkBuffer* buffer,
                         VmaAllocation* allocation, VmaAllocationInfo* allocationOut) {
    return createCounted(allocator, allocationInfo, gpuMemoryUseOfBuffer(bufferInfo->usage),
                         allocationOut,
                         [&](const VmaAllocationCreateInfo* tagged, VmaAllocationInfo* info) {
                             return vmaCreateBuffer(allocator, bufferInfo, tagged, buffer,
                                                    allocation, info);
                         });
}

VkResult gpuCreateImage(VmaAllocator allocator, const VkImageCreateInfo* imageInfo,
                        const VmaAllocationCreateInfo* allocationInfo, VkImage* image,
                        VmaAllocation* allocation, VmaAllocationInfo* allocationOut) {
    return createCounted(allocator, allocationInfo, gpuMemoryUseOfImage(imageInfo->usage),
                         allocationOut,
                         [&](const VmaAllocationCreateInfo* tagged, VmaAllocationInfo* info) {
                             return vmaCreateImage(allocator, imageInfo, tagged, image,
                                                   allocation, info);
                         });
}

void gpuDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation) {
    uncount(allocator, allocation);
    vmaDestroyBuffer(allocator, buffer, allocation);
}

void gpuDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation) {
    uncount(allocator, allocation);
    vmaDestroyImage(allocator, image, allocation);
}

void beginCountingGpuMemory(VmaAllocator allocator) {
    if (allocator == VK_NULL_HANDLE) return;
    auto counted = std::make_unique<Counted>();
    counted->allocator.store(allocator);
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator, &props);
    if (props) {
        counted->deviceLocalType.resize(props->memoryTypeCount);
        for (uint32_t t = 0; t < props->memoryTypeCount; ++t) {
            const uint32_t heap = props->memoryTypes[t].heapIndex;
            counted->deviceLocalType[t] =
                (props->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        }
    }
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.entries.push_back(std::move(counted));
}

void endCountingGpuMemory(VmaAllocator allocator) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    for (auto& e : r.entries) {
        if (e->allocator.load() == allocator) e->allocator.store(VK_NULL_HANDLE);
    }
}

std::optional<GpuMemoryByUse> countedGpuMemory(VmaAllocator allocator) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    for (const auto& e : r.entries) {
        if (allocator == VK_NULL_HANDLE || e->allocator.load() != allocator) continue;
        GpuMemoryByUse out;
        for (size_t i = 0; i < kGpuMemoryUseCount; ++i) {
            out.deviceLocalBytes[i] = e->deviceLocalBytes[i].load(std::memory_order_relaxed);
            out.hostBytes[i] = e->hostBytes[i].load(std::memory_order_relaxed);
        }
        out.allocations = static_cast<size_t>(e->allocations.load(std::memory_order_relaxed));
        return out;
    }
    return std::nullopt;
}

} // namespace rendering
} // namespace wowee
