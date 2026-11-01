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

/// What the GPU cull kept in one extracted frame: the slots of the instances
/// it kept, each with the id the slot held then (slot, id, slot, id, ...).
struct CullVerdict {
    uint64_t sceneIdentity = 0;
    uint32_t serial = 0;
    std::vector<uint32_t> kept;
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
    /// The terrain chunks the extraction walked to find the drawn ones.
    uint32_t terrainChunks = 0;
    /// The world's M2s in the frame recorded: candidates sent to the GPU
    /// cull, instances drawn and the skinned among them, bone matrices
    /// uploaded, sun shadow candidates kept, particle quads, and instances
    /// changed (placed, moved, hidden, ended) since the frame before.
    uint32_t m2CullCandidates = 0, m2Drawn = 0, m2Skinned = 0, m2BoneMatrices = 0;
    uint32_t m2SunCasters = 0, m2ParticleQuads = 0, m2InstanceChanges = 0;
    /// m2InstanceChanges by why; the rest were reclassified or got a new
    /// copy of their model.
    uint32_t m2Placed = 0, m2Moved = 0, m2ShownOrHidden = 0, m2Finished = 0;
    uint64_t terrainTriangles = 0;
    uint32_t rtMeshes = 0, rtInstances = 0;
    uint64_t rtTriangles = 0, rtGpuBytes = 0, rtSourceBytes = 0;
    uint32_t width = 0, height = 0;
    uint32_t wmoDistanceCulled = 0, wmoPortalCulled = 0, wmoFrustumCulled = 0;
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
    std::vector<CullVerdict> culling;
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
