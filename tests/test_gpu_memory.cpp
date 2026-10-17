// What the video memory is used for, read from the allocator's own record of
// every allocation: the detailed map vmaBuildStatsString writes. Each
// allocation counts once, under what its usage flags say it was created for,
// in VRAM or system memory by the heap its memory type lives on.
#include <catch_amalgamated.hpp>

#include "rendering/gpu_memory.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

using wowee::rendering::classifyGpuAllocations;
using wowee::rendering::GpuMemoryUse;

namespace {

constexpr uint64_t kMiB = 1024ull * 1024;

std::string usage(uint64_t flags) { return std::to_string(flags); }

/// The shape of VMA 3's detailed map: the heaps, then each memory type's
/// blocks with their suballocations and its dedicated allocations. Type 0 is
/// device-local, type 1 host-visible.
std::string detailedMap() {
    const std::string sampled = usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const std::string depth = usage(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const std::string vertex = usage(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    const std::string blas = usage(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    const std::string staging = usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    return R"({
  "General": {"API": "Vulkan", "GPU": "Test"},
  "Total": {"BlockCount": 2, "AllocationCount": 6},
  "MemoryInfo": {
    "Heap 0": {"Flags": ["DEVICE_LOCAL"], "Size": 12884901888,
               "MemoryPools": {"Type 0": {"Flags": ["DEVICE_LOCAL"], "Stats": {"BlockCount": 1}}}},
    "Heap 1": {"Flags": [], "Size": 34359738368,
               "MemoryPools": {"Type 1": {"Flags": ["HOST_VISIBLE"], "Stats": {"BlockCount": 1}}}}
  },
  "DefaultPools": {
    "Type 0": {
      "PreferredBlockSize": 268435456,
      "Blocks": {
        "0": {"MapRefCount": 0, "TotalBytes": 268435456, "UnusedBytes": 1048576,
              "Allocations": 3, "UnusedRanges": 1,
              "Suballocations": [
                {"Offset": 0, "Type": "IMAGE_OPTIMAL", "Size": )" + std::to_string(4 * kMiB) +
           R"(, "Usage": )" + sampled + R"(, "Name": "img 1024x1024 mips=11 fmt=133 from m2_renderer.cpp:12"},
                {"Offset": 4194304, "Type": "FREE", "Size": 1048576},
                {"Offset": 5242880, "Type": "BUFFER", "Size": )" + std::to_string(2 * kMiB) +
           R"(, "Usage": )" + vertex + R"(},
                {"Offset": 7340032, "Type": "BUFFER", "Size": )" + std::to_string(3 * kMiB) +
           R"(, "Usage": )" + blas + R"(}
              ]}
      },
      "DedicatedAllocations": [
        {"Type": "IMAGE_OPTIMAL", "Size": )" + std::to_string(32 * kMiB) + R"(, "Usage": )" + depth +
           R"(, "Name": "depth"}
      ]
    },
    "Type 1": {
      "PreferredBlockSize": 268435456,
      "Blocks": {
        "0": {"TotalBytes": 268435456, "UnusedBytes": 0, "Allocations": 1, "UnusedRanges": 0,
              "Suballocations": [
                {"Offset": 0, "Type": "BUFFER", "Size": )" + std::to_string(5 * kMiB) +
           R"(, "Usage": )" + staging + R"(}
              ]}
      },
      "DedicatedAllocations": []
    }
  },
  "CustomPools": {}
})";
}

} // namespace

TEST_CASE("each allocation counts under what it was created for, on the heap it lives in",
          "[gpu_memory]") {
    const auto byUse = classifyGpuAllocations(detailedMap(), {true, false});
    REQUIRE(byUse);

    // Sampled only: a texture. Drawn into: a render target, whatever else it
    // is sampled as.
    CHECK(byUse->deviceLocal(GpuMemoryUse::Textures) == 4 * kMiB);
    CHECK(byUse->deviceLocal(GpuMemoryUse::RenderTargets) == 32 * kMiB);
    // A vertex buffer the ray tracer builds from is still geometry, counted
    // once; the acceleration structure itself is the ray tracer's.
    CHECK(byUse->deviceLocal(GpuMemoryUse::Geometry) == 2 * kMiB);
    CHECK(byUse->deviceLocal(GpuMemoryUse::RayTracing) == 3 * kMiB);
    // The staging buffer is in system memory, not VRAM.
    CHECK(byUse->deviceLocal(GpuMemoryUse::OtherBuffers) == 0);
    CHECK(byUse->hostBytes[static_cast<size_t>(GpuMemoryUse::OtherBuffers)] == 5 * kMiB);
    // The free range is not an allocation.
    CHECK(byUse->allocations == 5);
}

TEST_CASE("text that is not the allocator's map is no breakdown at all", "[gpu_memory]") {
    CHECK_FALSE(classifyGpuAllocations("{\"DefaultPools\": {", {true}));
    CHECK_FALSE(classifyGpuAllocations("", {true}));
}
