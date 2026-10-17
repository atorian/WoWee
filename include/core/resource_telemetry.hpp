#pragma once

#include "core/cache_stats.hpp"
#include "core/process_stats.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee {
namespace core {

/// The last `capacity` values pushed, oldest first: one graph's worth of
/// history. A value that was not measured is pushed as NaN and kept, so the
/// samples either side stay where they were in time.
class SampleRing {
public:
    explicit SampleRing(size_t capacity);

    void push(float value);
    [[nodiscard]] size_t size() const { return count_; }
    [[nodiscard]] size_t capacity() const { return values_.size(); }
    /// The i-th value held, 0 being the oldest.
    [[nodiscard]] float at(size_t i) const;
    /// The values that were measured, oldest first.
    [[nodiscard]] std::vector<float> measured() const;

private:
    std::vector<float> values_;
    size_t next_ = 0;
    size_t count_ = 0;
};

/// How a run of frames went.
struct FrameSummary {
    uint32_t frames = 0;
    float averageFps = 0.0f;
    /// The rate of the slowest frame.
    float minimumFps = 0.0f;
    /// The frame time 99% of frames were no slower than.
    float p99FrameMs = 0.0f;
};

/// One reading of what the client holds: the operating system's figures,
/// the caches as their owners count them, and what the frame and the world
/// connection were doing.
struct ResourceSample {
    /// Seconds on the caller's clock.
    double timeSeconds = 0.0;
    /// The frames since the previous sample. Filled by ResourceTelemetry.
    FrameSummary frames;
    /// The world server's round trip, while a character is in the world.
    std::optional<uint32_t> latencyMs;
    ProcessStats process;

    struct Cache {
        std::string name;
        CacheStats stats;
    };
    struct Count {
        std::string name;
        uint64_t value = 0;
    };
    std::vector<Cache> caches;
    /// What is held where only the number is known: models, jobs in flight.
    std::vector<Count> counts;

    /// The device's video memory, as the driver counts it.
    struct VideoMemory {
        /// What the driver lets the client hold in its largest VRAM heap.
        uint64_t budgetBytes = 0;
        /// What the client holds there now.
        uint64_t usageBytes = 0;
        /// What it is used for - render targets, textures, normal maps,
        /// geometry - in bytes, as last broken down. Empty until it has been.
        std::vector<Count> parts;
    };
    std::optional<VideoMemory> videoMemory;
};

/// The figures kept as graphs, one value a sample.
enum class ResourceSeries : size_t {
    FrameRate,
    LatencyMs,
    WorkingSetMB,
    PrivateMB,
    SystemCommitUsedMB,
    Threads,
    VideoMemoryMB,
    Count_,
};

/// Where the client is close enough to running out of something that a
/// crash log should already say so.
struct ResourceThresholds {
    /// Each thread reserves a stack; past this the process is leaking them.
    uint32_t maxThreads = 512;
    /// The system's commit left below this: the next large allocation fails
    /// and the client aborts with bad_alloc.
    uint64_t minSystemCommitFreeBytes = 2ull * 1024 * 1024 * 1024;
    /// The client's own commit above this share of the physical memory: the
    /// machine pages it out from under itself.
    double maxPrivateShareOfPhysical = 0.5;
    /// Video memory in use above this share of the driver's budget: the next
    /// render target or texture may not fit, and the driver starts moving the
    /// client's memory out to system RAM.
    double maxVideoMemoryShareOfBudget = 0.95;
};

/// A threshold crossed or left.
struct ResourceAlert {
    enum class Kind { Threads, SystemCommit, PrivateCommit, VideoMemory };
    Kind kind = Kind::Threads;
    /// True when it was crossed, false when the figure came back under it.
    bool crossed = true;
    std::string message;
};

/// What the client's resources have been over the last ten minutes, and the
/// lines that say so in the log.
///
/// Fed a frame at a time and a sample at a time by its owner, which reads
/// the figures; this keeps the history, summarises the frames, decides when
/// the periodic line is due and when a threshold has been crossed - once per
/// crossing, however many samples stay over it.
class ResourceTelemetry {
public:
    struct Config {
        /// Samples kept per graph.
        size_t historySamples = 600;
        /// How often the resource line is due.
        double logIntervalSeconds = 60.0;
        ResourceThresholds thresholds;
    };

    explicit ResourceTelemetry(Config config);

    /// A frame took this long.
    void noteFrame(float seconds);

    struct Recorded {
        /// Thresholds crossed or left by this sample.
        std::vector<ResourceAlert> alerts;
        /// The resource line, when it is due.
        std::optional<std::string> logLine;
    };
    /// Keep a sample. Its frame summary is of the frames noted since the
    /// previous one; the log line's is of those since the previous line.
    Recorded record(ResourceSample sample);

    /// The latest sample, or null before the first.
    [[nodiscard]] const ResourceSample* latest() const;
    [[nodiscard]] const SampleRing& series(ResourceSeries which) const;

    /// The resource line for a sample, with this frame summary.
    [[nodiscard]] static std::string describe(const ResourceSample& sample,
                                              const FrameSummary& frames);

private:
    struct FrameWindow {
        std::vector<float> seconds;
        [[nodiscard]] FrameSummary summary() const;
    };
    /// Whether one figure is over its limit, said once a crossing.
    struct Watch {
        bool over = false;
        /// Crossed when value > limit; left when value < limit - margin.
        std::optional<bool> update(double value, double limit, double margin);
    };
    void checkThresholds(const ResourceSample& sample, const std::string& line,
                         std::vector<ResourceAlert>& alerts);

    Config config_;
    FrameWindow sampleFrames_;
    FrameWindow logFrames_;
    std::optional<double> lastLogAt_;
    std::optional<ResourceSample> latest_;
    /// One per ResourceSeries.
    std::vector<SampleRing> series_;
    Watch threads_;
    Watch systemCommit_;
    Watch privateCommit_;
    Watch videoMemory_;
};

/// A byte count as the HUD and the log write it: "512 MB", "1.25 GB".
[[nodiscard]] std::string formatBytes(uint64_t bytes);

} // namespace core
} // namespace wowee
