#include "core/resource_monitor.hpp"
#include "collision/collision_world.hpp"

#include "core/env_flag.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/wmo_renderer.hpp"

#include <algorithm>
#include <cstdlib>

namespace wowee::core {

namespace {

constexpr double kSampleIntervalSeconds = 1.0;
/// How often the allocator's detailed map is built, when anything asks for it.
/// It walks every allocation the allocator holds.
constexpr double kVideoMapIntervalSeconds = 10.0;
/// How often the client's own memory is counted by owner.
constexpr double kMemoryIntervalSeconds = 10.0;

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
                    gpuMemoryDumpPath_, " every ", kVideoMapIntervalSeconds, " s");
    }
    memoryByOwner_ = envFlagEnabled("WOWEE_MEMORY_BY_OWNER", false);
    if (memoryByOwner_) {
        LOG_WARNING("WOWEE_MEMORY_BY_OWNER: the client's memory is counted by owner every ",
                    kMemoryIntervalSeconds, " s; each count walks every model, tile and M2 "
                    "instance in one frame, some 30 ms in a city");
    }
    checkCounts_ = envFlagEnabled("WOWEE_GPU_MEMORY_CHECK", false);
    if (checkCounts_) {
        LOG_WARNING("WOWEE_GPU_MEMORY_CHECK: video memory counted by use is checked against the "
                    "allocator's detailed map every ", kVideoMapIntervalSeconds,
                    " s; each check stalls a frame for as long as the map takes");
    }
}

void ResourceMonitor::checkCountsAgainstMap(const rendering::VkContext& vk,
                                            const rendering::GpuMemoryByUse& counted) {
    const auto started = std::chrono::steady_clock::now();
    const auto mapped = vk.gpuMemoryByUseFromMap();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (!mapped) return;   // the map not parsing is said where it is parsed
    // Loading threads allocate while the map is built, so a little drift
    // between the two readings is the counts and the map disagreeing about
    // when, not about what. More than that is an allocation the counts never
    // saw, or one freed without being taken off.
    constexpr uint64_t kSlackBytes = 64ull << 20;
    bool agree = true;
    for (size_t i = 0; i < rendering::kGpuMemoryUseCount; ++i) {
        const auto use = static_cast<rendering::GpuMemoryUse>(i);
        const uint64_t a = counted.deviceLocal(use), b = mapped->deviceLocal(use);
        if ((a > b ? a - b : b - a) <= kSlackBytes) continue;
        agree = false;
        LOG_WARNING("Video memory check: ", rendering::gpuMemoryUseName(use), " counted ",
                    a >> 20, " MB, the allocator's map says ", b >> 20,
                    " MB - an allocation made or freed past gpu_allocations.hpp");
    }
    LOG_WARNING("Video memory check: ", counted.allocations, " allocations counted, ",
                mapped->allocations, " in the map", agree ? ", every use agrees" : "",
                " (the map took ", ms, " ms)");
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

    // Counted as allocations are made and freed, so read every sample.
    const auto byUse = vk->gpuMemoryByUse();
    if (!byUse && !warnedUncounted_) {
        warnedUncounted_ = true;
        LOG_WARNING("Video memory: the allocator's use is not being counted; the breakdown by use "
                    "is left out of the resource line");
    }
    std::vector<ResourceSample::Count> parts;
    if (byUse) {
        for (size_t i = 0; i < rendering::kGpuMemoryUseCount; ++i) {
            const auto use = static_cast<rendering::GpuMemoryUse>(i);
            parts.push_back({rendering::gpuMemoryUseName(use), byUse->deviceLocal(use)});
        }
    }

    // The detailed map walks every allocation - 150-300 ms in a city - so it
    // is built only when asked for: to be written out, or to check the counts.
    if ((checkCounts_ || !gpuMemoryDumpPath_.empty()) &&
        (!lastMapAt_ || now - *lastMapAt_ >= kVideoMapIntervalSeconds)) {
        lastMapAt_ = now;
        if (checkCounts_ && byUse) checkCountsAgainstMap(*vk, *byUse);
        if (!gpuMemoryDumpPath_.empty()) (void)vk->writeGpuMemoryMap(gpuMemoryDumpPath_);
    }
    out.videoMemory = ResourceSample::VideoMemory{
        .budgetBytes = heap->budgetBytes, .usageBytes = heap->usageBytes, .parts = std::move(parts)};

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
    if (memoryByOwner_ && (!lastMemoryAt_ || now - *lastMemoryAt_ >= kMemoryIntervalSeconds)) {
        lastMemoryAt_ = now;
        sampleMemory(*renderer);
    }
    out.memory = memoryParts_;
    return out;
}

void ResourceMonitor::sampleMemory(const rendering::Renderer& renderer) {
    // Walks every model, tile and M2 instance - 200,000 of those in a city -
    // so it is done only when asked for, every few seconds, and timed.
    const auto started = std::chrono::steady_clock::now();
    memoryParts_.clear();
    if (const auto* characters = renderer.getCharacterRenderer()) {
        const auto m = characters->modelMemory();
        memoryParts_.push_back({"character models", m.totalBytes});
        memoryParts_.push_back({"  of which geometry", m.geometryBytes});
        memoryParts_.push_back({"  of which bone tracks", m.boneTrackBytes});
    }
    if (const auto* m2 = renderer.getM2Renderer()) {
        memoryParts_.push_back({"M2 models", m2->modelDataBytes()});
        memoryParts_.push_back({"M2 instances", m2->instanceBytes()});
    }
    // Placements, ground and every shape, M2 and WMO, counted once.
    memoryParts_.push_back({"collision world", renderer.collision().bytes()});
    if (const auto* terrain = renderer.getTerrainManager()) {
        const auto tiles = terrain->tileMemory();
        memoryParts_.push_back({"terrain tiles", tiles.terrainBytes});
    }
    if (renderer.getRtScene()) {
        memoryParts_.push_back({"ray tracing sources", renderer.renderStatistics().rtSourceBytes});
    }
    if (const auto* vk = renderer.getVkContext()) {
        if (const auto byUse = vk->gpuMemoryByUse()) {
            uint64_t host = 0;
            for (uint64_t b : byUse->hostBytes) host += b;
            memoryParts_.push_back({"Vulkan host memory", host});
        }
    }
    if (luaHeapBytes_) memoryParts_.push_back({"Lua heap", *luaHeapBytes_});
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (ms > 5.0) {
        LOG_WARNING("Resource line: counting the client's memory by owner took ", ms,
                    " ms of one frame");
    }
}

} // namespace wowee::core
