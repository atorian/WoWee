#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wowee::rendering {

struct FrameStage {
    std::string group, name;
    float milliseconds = 0;
};

struct CullAnswer {
    uint32_t id = 0;
    bool visible = false;
    uint64_t sceneIdentity = 0, gpuFrame = 0;
};

struct VisibilityAnswer {
    uint64_t gpuFrame = 0, worldEpoch = 0;
    float value = 0;
};

/// A mapped readback buffer remains owned until the last reader releases it.
struct CapturedImage {
    std::shared_ptr<const uint8_t> pixels;
    uint32_t width = 0, height = 0, stride = 0;
    int64_t pts = 0;
    uint64_t generation = 0, gpuFrame = 0;
    std::string screenshot;
};

struct RenderStatistics {
    double renderMs = 0, terrainMs = 0, wmoMs = 0, m2Ms = 0;
    uint32_t terrainDrawn = 0, terrainCulled = 0, m2Draws = 0, wmoDraws = 0, wmoShadows = 0;
    uint64_t terrainTriangles = 0;
    uint32_t rtMeshes = 0, rtInstances = 0;
    uint64_t rtTriangles = 0, rtGpuBytes = 0, rtSourceBytes = 0;
    uint32_t width = 0, height = 0;
    uint32_t wmoDistanceCulled = 0, wmoPortalCulled = 0;
    uint32_t waterDrawn = 0;
    std::string sunDisc = "pending", moonDisc = "pending";
    bool framegenActive = false, framegenReady = false;
    std::string framegenPath, framegenError;
    size_t framegenDispatches = 0, upscaleDispatches = 0, framegenFallbacks = 0;
};

/// Written by recording, consumed once at the next game-frame boundary.
/// Recording numbers and GPU submission numbers are separate: a minimized
/// window may complete a recording attempt without submitting a GPU frame.
struct FrameResults {
    uint64_t gameFrame = 0, recording = 0, submittedFrame = 0;
    uint64_t gpuTimingSerial = 0, gpuTimingFrame = 0;
    std::vector<FrameStage> cpu;
    std::vector<std::pair<std::string, double>> gpu;
    VisibilityAnswer exposure, skyView, sun, moon;
    RenderStatistics statistics;
    std::vector<CullAnswer> culling;
    std::vector<CapturedImage> images;
    std::vector<uint64_t> captureDrops;
    std::vector<std::pair<std::string, bool>> screenshots;
};

class FrameResultMailbox {
public:
    void publish(FrameResults results) {
        std::lock_guard lock(mutex_);
        pending_.push_back(std::move(results));
    }
    std::vector<FrameResults> take() {
        std::lock_guard lock(mutex_);
        std::vector<FrameResults> ready;
        ready.swap(pending_);
        return ready;
    }
private:
    std::mutex mutex_;
    std::vector<FrameResults> pending_;
};

} // namespace wowee::rendering
