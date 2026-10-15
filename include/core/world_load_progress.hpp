#pragma once

#include <algorithm>
#include <string>

namespace wowee::core {

enum class WorldLoadResult { Pending, Ready, Failed };

struct WorldLoadStatus {
    WorldLoadResult result = WorldLoadResult::Pending;
    std::string reason = "Preparing world...";
    float progress = 0.0f;
};

/// The former terrain loop's completion/stall rules, observed once per frame.
class WorldTileProgress {
public:
    WorldLoadStatus observe(float elapsed, int loaded, int remaining) {
        if (remaining != lastRemaining_) {
            lastRemaining_ = remaining;
            lastChange_ = elapsed;
        }
        if (remaining == 0) return {WorldLoadResult::Ready, "Terrain streamed", 0.85f};
        if (elapsed > 60.0f)
            return {WorldLoadResult::Ready, "Terrain preload timed out; remaining tiles will stream in game", 0.85f};
        if (elapsed - lastChange_ > 10.0f)
            return {WorldLoadResult::Ready, "Terrain preload stalled; remaining tiles will stream in game", 0.85f};
        return {WorldLoadResult::Pending,
                "Loading terrain... " + std::to_string(loaded) + " / " + std::to_string(loaded + remaining) + " tiles",
                0.35f + 0.5f * loaded / std::max(1, loaded + remaining)};
    }
private:
    int lastRemaining_ = -1;
    float lastChange_ = 0.0f;
};

/// Keep the old warmup criteria, including its degraded completion paths.
/// Upload completion replaces the final blocking waitAllUploads().
class WorldWarmupProgress {
public:
    WorldLoadStatus observe(float elapsed, bool queuesEmpty, bool groundNearSpawn,
                            int loadedTiles, bool uploadsComplete) {
        idleFrames_ = queuesEmpty ? idleFrames_ + 1 : 0;
        const bool tileFallback = !groundNearSpawn && elapsed >= 5.0f && loadedTiles >= 4;
        const bool groundReady = groundNearSpawn || tileFallback;
        const bool converged = elapsed >= 2.0f && idleFrames_ >= 5 && groundReady;
        const bool timedOut = elapsed >= 25.0f;
        if ((converged || timedOut) && uploadsComplete) {
            if (timedOut && !groundReady)
                return {WorldLoadResult::Ready, "Warmup hit hard cap; ground is not ready, player may fall through world", 1.0f};
            if (timedOut && !converged)
                return {WorldLoadResult::Ready, "Warmup hit hard cap; entering world with pending work", 1.0f};
            if (tileFallback)
                return {WorldLoadResult::Ready, "Warmup accepted tile-count fallback; spawn floor height is unavailable", 1.0f};
            return {WorldLoadResult::Ready, "World ready", 1.0f};
        }
        return {WorldLoadResult::Pending,
                (converged || timedOut) && !uploadsComplete ? "Finishing GPU uploads..." : "Finalizing world sync...",
                0.97f + std::clamp(elapsed / 25.0f, 0.0f, 1.0f) * 0.025f};
    }
private:
    int idleFrames_ = 0;
};

} // namespace wowee::core
