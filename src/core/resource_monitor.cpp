#include "core/resource_monitor.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/wmo_renderer.hpp"

#include <algorithm>
#include <cstdlib>

namespace wowee::core {

namespace {

constexpr double kSampleIntervalSeconds = 1.0;
/// How often video memory is broken down by use. The breakdown walks every
/// allocation the allocator holds.
constexpr double kVideoBreakdownIntervalSeconds = 10.0;

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
      started_(std::chrono::steady_clock::now()) {
    if (const char* path = std::getenv("WOWEE_GPU_MEMORY_DUMP"); path && *path) {
        gpuMemoryDumpPath_ = path;
        LOG_WARNING("WOWEE_GPU_MEMORY_DUMP: the allocator's detailed map is written to ",
                    gpuMemoryDumpPath_, " every ", kVideoBreakdownIntervalSeconds, " s");
    }
}

void ResourceMonitor::frameFinished(std::optional<float> frameSeconds,
                                    const rendering::Renderer* renderer,
                                    const pipeline::AssetManager* assets,
                                    std::optional<uint32_t> latencyMs) {
    const auto now = std::chrono::steady_clock::now();
    if (frameSeconds) telemetry_.noteFrame(*frameSeconds);

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

void ResourceMonitor::sampleVideoMemory(const rendering::Renderer& renderer, double now,
                                        ResourceSample& out) {
    const rendering::VkContext* vk = renderer.getVkContext();
    if (!vk) return;
    const auto heap = vk->deviceLocalHeap();
    if (!heap) return;

    if (!lastVideoBreakdownAt_ || now - *lastVideoBreakdownAt_ >= kVideoBreakdownIntervalSeconds) {
        lastVideoBreakdownAt_ = now;
        const auto started = std::chrono::steady_clock::now();
        if (const auto byUse = vk->gpuMemoryByUse()) {
            videoMemoryParts_.clear();
            for (size_t i = 0; i < rendering::kGpuMemoryUseCount; ++i) {
                const auto use = static_cast<rendering::GpuMemoryUse>(i);
                videoMemoryParts_.push_back({rendering::gpuMemoryUseName(use), byUse->deviceLocal(use)});
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            LOG_DEBUG("Video memory broken down over ", byUse->allocations, " allocations in ", ms, " ms");
        }
        if (!gpuMemoryDumpPath_.empty()) (void)vk->writeGpuMemoryMap(gpuMemoryDumpPath_);
    }
    out.videoMemory = ResourceSample::VideoMemory{
        .budgetBytes = heap->budgetBytes, .usageBytes = heap->usageBytes, .parts = videoMemoryParts_};

    // The texture cache counts its own, every second: which forms the
    // textures in video memory take, and what is waiting for its frames.
    const auto textures = vk->textures().stats();
    out.caches.push_back({"texture cache", {textures.entries, textures.bytes, textures.budgetBytes}});
    for (size_t i = 0; i < rendering::kTextureFormCount; ++i) {
        const auto form = static_cast<rendering::TextureForm>(i);
        out.videoMemory->parts.push_back({std::string("cache: ") + rendering::textureFormName(form),
                                          textures.form(form).bytes});
    }
    out.videoMemory->parts.push_back({"cache: unreferenced", textures.idleBytes});
    out.videoMemory->parts.push_back({"cache: waiting for frames", textures.retiringBytes});
    if (textures.refusals > 0) out.counts.push_back({"textures refused", textures.refusals});
}

ResourceSample ResourceMonitor::sample(const rendering::Renderer* renderer,
                                       const pipeline::AssetManager* assets,
                                       std::optional<uint32_t> latencyMs, double now) {
    ResourceSample out;
    out.timeSeconds = now;
    out.latencyMs = latencyMs;
    if (auto process = sampler_.latest()) out.process = *process;

    if (assets) {
        out.caches.push_back({"asset files", assets->fileCacheStats()});
        out.counts.push_back({"DBC tables", assets->dbcCacheEntries()});
    }
    if (!renderer) return out;
    sampleVideoMemory(*renderer, now, out);
    if (const auto* terrain = renderer->getTerrainManager()) {
        out.counts.push_back({"terrain tiles loaded",
                              static_cast<uint64_t>(terrain->getLoadedTileCount())});
        out.counts.push_back({"terrain tiles in flight",
                              static_cast<uint64_t>(terrain->getRemainingTileCount())});
    }
    if (const auto* m2 = renderer->getM2Renderer()) {
        out.counts.push_back({"M2 models", m2->getModelCount()});
    }
    if (const auto* wmo = renderer->getWMORenderer()) {
        out.counts.push_back({"WMO models", wmo->getModelCount()});
    }
    if (const auto* characters = renderer->getCharacterRenderer()) {
        out.counts.push_back({"character models", characters->getModelCount()});
        out.counts.push_back({"normal-map jobs", static_cast<uint64_t>(characters->pendingNormalMapJobs())});
    }
    return out;
}

} // namespace wowee::core
