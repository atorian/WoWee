#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace wowee {
namespace rendering {

/// Where the pipeline cache is kept: in the client's own cache directory,
/// beside the WMO floor caches, relative to the working directory the client
/// runs in (its bin folder). It used to go under %APPDATA%, on the system
/// drive, where it does not belong with the rest of what the client keeps.
inline const std::filesystem::path kPipelineCachePath = "cache/pipeline_cache.bin";

/// The device and driver a pipeline cache is good for: the fields of
/// VkPipelineCacheHeaderVersionOne that a driver compares before using one.
struct PipelineCacheIdentity {
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    std::array<uint8_t, VK_UUID_SIZE> cacheUuid{};

    [[nodiscard]] static PipelineCacheIdentity of(const VkPhysicalDeviceProperties& props);
};

/// The saved cache at `path`, if it was written by this device and driver;
/// empty when there is none, it cannot be read, or it is someone else's - a
/// driver update changes the UUID - with the reason logged. Checked here
/// rather than trusted to the driver: some have crashed on a stale blob
/// instead of refusing it.
[[nodiscard]] std::vector<char> loadPipelineCacheFile(const std::filesystem::path& path,
                                                      const PipelineCacheIdentity& device);

/// Writes `data` to `path` whole or not at all: into a file beside it, then
/// renamed over it, so a client killed mid-write leaves the last good cache.
/// Logs and returns false on failure.
bool savePipelineCacheFile(const std::filesystem::path& path, const std::vector<char>& data);

} // namespace rendering
} // namespace wowee
