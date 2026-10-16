#pragma once

#include "core/process_stats.hpp"
#include "core/resource_telemetry.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

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
                                        std::optional<uint32_t> latencyMs, double now) const;

    bool loud_;
    ResourceTelemetry telemetry_;
    ProcessStatsSampler sampler_;
    std::chrono::steady_clock::time_point started_;
    std::optional<std::chrono::steady_clock::time_point> lastFrame_;
    double lastSampleAt_ = 0.0;
};

} // namespace core
} // namespace wowee
