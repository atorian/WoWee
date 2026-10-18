#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace core {

/// How a run of timings is spread: the typical one, the slow tail and the
/// worst. Percentiles are nearest-rank: p95 is a value that was measured, the
/// one 95% of the run was no slower than.
struct TimeDistribution {
    uint32_t count = 0;
    double totalMs = 0.0;
    float meanMs = 0.0f;
    float p50Ms = 0.0f;
    float p95Ms = 0.0f;
    float p99Ms = 0.0f;
    float maxMs = 0.0f;
};

/// The distribution of `samplesMs`. Reorders them; empty gives all zeros.
[[nodiscard]] TimeDistribution distributionOf(std::vector<float>& samplesMs);

/// The frame times a hitch is counted against: a frame slower than 1/120,
/// 1/60 and 1/30 of a second missed that refresh rate.
inline constexpr float kHitchLimitsMs[] = {8.3f, 16.7f, 33.3f};

/// Where the frames of a stretch of play went: the frame time and its tail,
/// the CPU stages each frame is made of, and the GPU's passes.
///
/// The averages alone said a city cost 29 ms a frame; they could not say
/// whether that was every frame or one frame in ten taking 100. This keeps
/// every sample of the stretch and reports the distribution, the frames that
/// missed 120, 60 and 30 fps, and each stage's and pass's 95th percentile
/// beside its average and its worst.
///
/// A stage named "parent/child" is part of "parent" and is reported under
/// it, indented: the renderer's beginFrame is broken down that way.
class FrameBudget {
public:
    /// A stage of the current frame took this long. A stage noted more than
    /// once in a frame is a sample each time.
    void noteStage(std::string_view name, float milliseconds);
    /// The GPU's timings of one frame, in the order the passes ran, as
    /// (label, milliseconds since the previous mark).
    void noteGpuFrame(const std::vector<std::pair<const char*, double>>& passes);
    /// A frame ended, this long after the one before.
    void frameFinished(float frameMilliseconds);

    [[nodiscard]] uint32_t frames() const { return static_cast<uint32_t>(frameMs_.size()); }
    [[nodiscard]] uint32_t gpuFrames() const { return gpuFrames_; }
    /// The frame times so far.
    [[nodiscard]] TimeDistribution frameTimes() const;
    /// Frames slower than `limitMs`.
    [[nodiscard]] uint32_t framesOver(float limitMs) const;

    /// The report, a line each, over `windowMs` of wall time. `title` opens
    /// the first line ("frame budget over 10s").
    [[nodiscard]] std::vector<std::string> report(std::string_view title, double windowMs) const;

    /// Start a new stretch.
    void reset();

private:
    struct Series {
        std::vector<float> samplesMs;
        double totalMs = 0.0;
    };
    void stageLines(const std::string& parent, int depth, double windowMs,
                    std::vector<std::string>& out) const;

    std::vector<float> frameMs_;
    std::map<std::string, Series, std::less<>> stages_;
    /// In the order the passes first ran.
    std::vector<std::pair<std::string, Series>> gpuPasses_;
    std::vector<float> gpuFrameMs_;
    uint32_t gpuFrames_ = 0;
};

} // namespace core
} // namespace wowee
