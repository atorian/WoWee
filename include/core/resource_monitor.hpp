#pragma once

#include "core/process_stats.hpp"
#include "core/resource_telemetry.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class Renderer; class VkContext; struct GpuMemoryByUse; }

namespace core {

/// The client's memory, threads, caches, frame rate and latency, read once a
/// second and kept for ten minutes: what the performance HUD draws and what
/// the resource line in the log says.
///
/// The line is written once a minute at info, every ten seconds at warning
/// under WOWEE_FRAME_PROFILE; a threshold crossed is a warning once per
/// crossing, so a log that ends in a crash says what was running out.
class ResourceMonitor {
public:
    explicit ResourceMonitor(bool frameProfile);

    /// End of a frame `frameSeconds` long - none for the first, which has no
    /// frame before it. The caches are read from their owners when a sample
    /// is due; either may be null before the world's renderers exist.
    void frameFinished(std::optional<float> frameSeconds, const rendering::Renderer* renderer,
                       const pipeline::AssetManager* assets, std::optional<uint32_t> latencyMs);

    [[nodiscard]] const ResourceTelemetry& telemetry() const { return telemetry_; }

    /// What the character's interface holds in Lua, as its engine counts it;
    /// empty with no interface. Told by whoever owns the interface, which this
    /// does not see.
    void noteLuaHeap(std::optional<uint64_t> bytes) { luaHeapBytes_ = bytes; }

private:
    /// The client's own memory by owner (ResourceSample::memory).
    void sampleMemory(const rendering::Renderer& renderer);
    std::vector<ResourceSample::Count> memoryParts_;
    /// WOWEE_MEMORY_BY_OWNER: walking the owners costs a frame some 30 ms in
    /// a city, so it is asked for rather than always done.
    bool memoryByOwner_ = false;
    std::optional<double> lastMemoryAt_;
    std::optional<uint64_t> luaHeapBytes_;

    [[nodiscard]] ResourceSample sample(const rendering::Renderer* renderer,
                                        const pipeline::AssetManager* assets,
                                        std::optional<uint32_t> latencyMs, double now);
    /// The device-local heap, and what it is used for.
    void sampleVideoMemory(const rendering::Renderer& renderer, double now, ResourceSample& out);
    /// WOWEE_GPU_MEMORY_CHECK: the counts against the allocator's own map.
    void checkCountsAgainstMap(const rendering::VkContext& vk,
                               const rendering::GpuMemoryByUse& counted);

    bool loud_;
    /// Where WOWEE_GPU_MEMORY_DUMP asks for the allocator's detailed map to be
    /// written every few seconds; empty when it does not.
    std::string gpuMemoryDumpPath_;
    bool checkCounts_ = false;
    bool warnedUncounted_ = false;
    /// When the detailed map was last built, for either of the two above.
    std::optional<double> lastMapAt_;
    ResourceTelemetry telemetry_;
    ProcessStatsSampler sampler_;
    std::chrono::steady_clock::time_point started_;
    double lastSampleAt_ = 0.0;
};

} // namespace core
} // namespace wowee
