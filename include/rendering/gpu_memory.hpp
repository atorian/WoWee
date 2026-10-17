#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace wowee {
namespace rendering {

/// One memory heap of the device, as the driver reports it.
struct GpuHeapReading {
    uint32_t index = 0;
    /// VRAM, as opposed to system memory the device can reach.
    bool deviceLocal = false;
    uint64_t sizeBytes = 0;
    /// What the driver will let this process hold in the heap. Read from
    /// VK_EXT_memory_budget where the device has it, which takes the desktop
    /// and every other process into account; without it the allocator's
    /// estimate of 80% of the heap.
    uint64_t budgetBytes = 0;
    /// What this process holds in the heap now, as the driver counts it.
    uint64_t usageBytes = 0;
    /// Whether the two figures above came from the driver.
    bool fromDriver = false;
};

/// What an allocation on the GPU was created to be, read from the usage
/// flags it was created with.
enum class GpuMemoryUse : uint8_t {
    /// Images drawn or computed into: colour and depth targets, storage images.
    RenderTargets,
    /// Images that are only ever sampled.
    Textures,
    /// Vertex and index buffers.
    Geometry,
    /// Acceleration structures and the buffers their builds read.
    RayTracing,
    /// Uniform, storage and staging buffers, and anything of unknown kind.
    OtherBuffers,
    Count_,
};

inline constexpr size_t kGpuMemoryUseCount = static_cast<size_t>(GpuMemoryUse::Count_);

[[nodiscard]] const char* gpuMemoryUseName(GpuMemoryUse use);

/// The bytes allocated for each use, split between VRAM and system memory.
struct GpuMemoryByUse {
    std::array<uint64_t, kGpuMemoryUseCount> deviceLocalBytes{};
    std::array<uint64_t, kGpuMemoryUseCount> hostBytes{};
    /// Allocations counted.
    size_t allocations = 0;

    [[nodiscard]] uint64_t deviceLocal(GpuMemoryUse use) const {
        return deviceLocalBytes[static_cast<size_t>(use)];
    }
};

/// Sort every live allocation in the allocator's detailed statistics - the
/// JSON vmaBuildStatsString writes with its detailed map - by the usage it was
/// created with.
///
/// That map is the one place that lists every allocation with its size and
/// usage flags, whichever of the renderer's hundred allocation sites made it,
/// so nothing has to be tagged where it is created and nothing can be missed
/// where it is freed. `memoryTypeIsDeviceLocal[i]` says whether memory type i
/// lives on a device-local heap.
///
/// Empty if the text is not that JSON.
[[nodiscard]] std::optional<GpuMemoryByUse> classifyGpuAllocations(
    std::string_view vmaDetailedStatsJson, const std::vector<bool>& memoryTypeIsDeviceLocal);

} // namespace rendering
} // namespace wowee
