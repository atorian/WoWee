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
namespace rendering { class Renderer; }

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

    /// End of a frame. The caches are read from their owners when a sample is
    /// due; either may be null before the world's renderers exist.
    void frameFinished(const rendering::Renderer* renderer, const pipeline::AssetManager* assets,
                       std::optional<uint32_t> latencyMs);

    [[nodiscard]] const ResourceTelemetry& telemetry() const { return telemetry_; }

private:
    [[nodiscard]] ResourceSample sample(const rendering::Renderer* renderer,
                                        const pipeline::AssetManager* assets,
                                        std::optional<uint32_t> latencyMs, double now);
    /// The device-local heap, and what it is used for as last broken down.
    void sampleVideoMemory(const rendering::Renderer& renderer, double now, ResourceSample& out);

    bool loud_;
    /// Where WOWEE_GPU_MEMORY_DUMP asks for the allocator's detailed map to be
    /// written each time video memory is broken down; empty when it does not.
    std::string gpuMemoryDumpPath_;
    /// Breaking video memory down walks every allocation, so it is done every
    /// few seconds and the parts are carried between.
    std::vector<ResourceSample::Count> videoMemoryParts_;
    std::optional<double> lastVideoBreakdownAt_;
    ResourceTelemetry telemetry_;
    ProcessStatsSampler sampler_;
    std::chrono::steady_clock::time_point started_;
    std::optional<std::chrono::steady_clock::time_point> lastFrame_;
    double lastSampleAt_ = 0.0;
};

} // namespace core
} // namespace wowee
