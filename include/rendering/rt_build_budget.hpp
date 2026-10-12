#pragma once

/// How much ray-tracing geometry to build this frame.
///
/// Arriving somewhere new queues every building, doodad and terrain chunk in
/// sight for an acceleration structure at once. Building a fixed share each
/// frame either crawls in an empty field or, in Dalaran, takes most of a
/// frame on its own: 170 ms of GPU time per frame for several seconds was
/// measured there. This sizes each frame's share from what the previous
/// frames cost instead: the GPU time a triangle took to build, and what the
/// rest of the frame took, so the queue drains as fast as the frame rate
/// target allows and no faster.
///
/// Pure arithmetic over measurements, so it is tested without a GPU.

#include <algorithm>
#include <cstdint>

namespace wowee::rendering {

class RtBuildBudget {
public:
    /// The fewest triangles queued work gets per frame, however busy the
    /// frame: the queue still drains, slowly, on a frame that is over target.
    static constexpr uint64_t kMinTriangles = 4096;
    static constexpr uint64_t kMaxTriangles = 4u << 20;

    struct Sample {
        uint64_t triangles = 0;  // built in the measured frame
        double buildMs = 0.0;    // GPU time of that frame's scene update
        double frameMs = 0.0;    // GPU time of that whole frame
    };

    explicit RtBuildBudget(double targetFrameMs = 1000.0 / 60.0) : targetMs_(targetFrameMs) {}

    void observe(const Sample& s) {
        if (s.triangles == 0) {
            // A frame with nothing to build measures the fixed part: the TLAS
            // and the tables, which are paid whatever the share.
            fixedMs_ += (s.buildMs - fixedMs_) * 0.1;
        } else if (s.triangles >= 1024 && s.buildMs > fixedMs_) {
            const double perTri = std::clamp((s.buildMs - fixedMs_) / double(s.triangles), 1e-8, 1e-2);
            msPerTriangle_ += (perTri - msPerTriangle_) * 0.3;
        }
        // What the frame costs without this frame's triangles.
        const double rest = s.frameMs - std::max(0.0, s.buildMs - fixedMs_);
        const double headroom = targetMs_ - rest - kSafetyMs;
        uint64_t next = kMinTriangles;
        if (headroom > 0.0) {
            next = static_cast<uint64_t>(std::min(headroom / msPerTriangle_, double(kMaxTriangles)));
        }
        // Back off at once; grow at most twofold a frame, so one quiet frame
        // does not license a share the next frame cannot afford.
        budget_ = std::clamp(std::min(next, budget_ * 2), kMinTriangles, kMaxTriangles);
    }

    [[nodiscard]] uint64_t budget() const { return budget_; }
    [[nodiscard]] double msPerTriangle() const { return msPerTriangle_; }

private:
    static constexpr double kSafetyMs = 1.0;

    double targetMs_;
    // Hardware builds run at tens of millions of triangles a second; start
    // pessimistic and let the first measurements correct it.
    double msPerTriangle_ = 1e-4;
    double fixedMs_ = 0.3;
    uint64_t budget_ = 32768;
};

}  // namespace wowee::rendering
