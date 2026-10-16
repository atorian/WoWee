#include "core/resource_monitor.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/wmo_renderer.hpp"

#include <algorithm>

namespace wowee::core {

namespace {

constexpr double kSampleIntervalSeconds = 1.0;

ResourceTelemetry::Config telemetryConfig(bool frameProfile) {
    ResourceTelemetry::Config config;
    // Ten minutes of graph at a sample a second.
    config.historySamples = 600;
    config.logIntervalSeconds = frameProfile ? 10.0 : 60.0;
    return config;
}

} // namespace

ResourceMonitor::ResourceMonitor(bool frameProfile)
    : loud_(frameProfile),
      telemetry_(telemetryConfig(frameProfile)),
      started_(std::chrono::steady_clock::now()) {}

void ResourceMonitor::frameFinished(const rendering::Renderer* renderer,
                                    const pipeline::AssetManager* assets,
                                    std::optional<uint32_t> latencyMs) {
    const auto now = std::chrono::steady_clock::now();
    if (lastFrame_) telemetry_.noteFrame(std::chrono::duration<float>(now - *lastFrame_).count());
    lastFrame_ = now;

    const double seconds = std::chrono::duration<double>(now - started_).count();
    if (seconds - lastSampleAt_ < kSampleIntervalSeconds) return;
    lastSampleAt_ = seconds;

    const auto recorded = telemetry_.record(sample(renderer, assets, latencyMs, seconds));
    for (const auto& alert : recorded.alerts) {
        if (alert.crossed) LOG_WARNING(alert.message); else LOG_INFO(alert.message);
    }
    if (recorded.logLine) {
        Logger::getInstance().at(loud_ ? LogLevel::WARNING : LogLevel::INFO, *recorded.logLine);
    }
}

ResourceSample ResourceMonitor::sample(const rendering::Renderer* renderer,
                                       const pipeline::AssetManager* assets,
                                       std::optional<uint32_t> latencyMs, double now) const {
    ResourceSample out;
    out.timeSeconds = now;
    out.latencyMs = latencyMs;
    if (auto process = sampler_.latest()) out.process = *process;

    if (assets) {
        out.caches.push_back({"asset files", assets->fileCacheStats()});
        out.counts.push_back({"DBC tables", assets->dbcCacheEntries()});
    }
    if (!renderer) return out;
    if (const auto* terrain = renderer->getTerrainManager()) {
        out.caches.push_back({"terrain tile cache", terrain->tileCacheStats()});
        out.counts.push_back({"terrain tiles loaded",
                              static_cast<uint64_t>(terrain->getLoadedTileCount())});
        out.counts.push_back({"terrain tiles in flight",
                              static_cast<uint64_t>(terrain->getRemainingTileCount())});
    }
    if (const auto* m2 = renderer->getM2Renderer()) {
        out.caches.push_back({"M2 textures", m2->textureCacheStats()});
        out.counts.push_back({"M2 models", m2->getModelCount()});
    }
    if (const auto* wmo = renderer->getWMORenderer()) {
        out.caches.push_back({"WMO textures", wmo->textureCacheStats()});
        out.counts.push_back({"WMO models", wmo->getModelCount()});
    }
    if (const auto* terrain = renderer->getTerrainRenderer()) {
        out.caches.push_back({"terrain textures", terrain->textureCacheStats()});
    }
    if (const auto* characters = renderer->getCharacterRenderer()) {
        out.caches.push_back({"character textures", characters->textureCacheStats()});
        out.counts.push_back({"character models", characters->getModelCount()});
        out.counts.push_back({"normal-map jobs",
                              static_cast<uint64_t>(std::max(characters->pendingNormalMapJobs(), 0))});
    }
    return out;
}

} // namespace wowee::core
