#include "rendering/performance_hud.hpp"
#include "collision/collision_world.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/skybox.hpp"
#include "rendering/sky_bodies.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/lightning.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/camera.hpp"
#include "core/client_shortcuts.hpp"
#include "core/resource_telemetry.hpp"
#include "ui/ui_colors.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <sstream>
#include <iomanip>

namespace wowee {
namespace rendering {

namespace {
    using namespace wowee::ui;
    constexpr ImVec4 kSectionHeader = {0.8f, 0.8f, 0.5f, 1.0f};
    const auto& kHelpText = colors::kGray;
    const auto& kTitle    = colors::kLightGray;

    constexpr double kMiB = 1024.0 * 1024.0;

    std::string megabytes(float mb) {
        return core::formatBytes(static_cast<uint64_t>(std::max(0.0f, mb) * kMiB));
    }
    std::string whole(float value) {
        char text[24];
        std::snprintf(text, sizeof(text), "%.0f", value);
        return text;
    }

    /// One figure over the history: its latest value, then a graph of the
    /// samples that measured it with the lowest and highest written over it.
    void plotSeries(const char* label, const core::SampleRing& ring,
                    const std::function<std::string(float)>& format) {
        const std::vector<float> values = ring.measured();
        if (values.empty()) {
            ImGui::TextColored(kHelpText, "%s: n/a", label);
            return;
        }
        const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
        ImGui::Text("%s: %s", label, format(values.back()).c_str());
        const std::string range = "min " + format(*lo) + "  max " + format(*hi);
        // A flat line still shows, a little above the floor of the graph.
        const float span = std::max(*hi - *lo, std::max(1.0f, *hi * 0.02f));
        ImGui::PlotLines((std::string("##") + label).c_str(), values.data(),
                         static_cast<int>(values.size()), 0, range.c_str(),
                         *lo - span * 0.1f, *hi + span * 0.1f, ImVec2(280, 40));
    }
} // namespace

PerformanceHUD::PerformanceHUD() {
}

PerformanceHUD::~PerformanceHUD() {
}

void PerformanceHUD::update(float deltaTime) {
    if (!enabled) {
        return;
    }

    // Store frame time
    frameTime = deltaTime;
    frameTimeHistory.push_back(deltaTime);

    // Keep history size limited
    while (frameTimeHistory.size() > MAX_FRAME_HISTORY) {
        frameTimeHistory.pop_front();
    }

    // Update stats periodically
    updateTimer += deltaTime;
    if (updateTimer >= UPDATE_INTERVAL) {
        updateTimer = 0.0f;
        calculateFPS();
    }
}

void PerformanceHUD::calculateFPS() {
    if (frameTimeHistory.empty()) {
        return;
    }

    // Current FPS (from last frame time)
    currentFPS = frameTime > 0.0001f ? 1.0f / frameTime : 0.0f;

    // Average FPS
    float sum = 0.0f;
    for (float ft : frameTimeHistory) {
        sum += ft;
    }
    float avgFrameTime = sum / frameTimeHistory.size();
    averageFPS = avgFrameTime > 0.0001f ? 1.0f / avgFrameTime : 0.0f;

    // Min/Max FPS (from last 2 seconds)
    minFPS = 10000.0f;
    maxFPS = 0.0f;
    for (float ft : frameTimeHistory) {
        if (ft > 0.0001f) {
            float fps = 1.0f / ft;
            minFPS = std::min(minFPS, fps);
            maxFPS = std::max(maxFPS, fps);
        }
    }
}

void PerformanceHUD::render(const Renderer* renderer, const Camera* camera) {
    if (!enabled || !renderer) {
        return;
    }

    // Set window position based on setting
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                            ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoFocusOnAppearing |
                            ImGuiWindowFlags_NoNav;

    const float PADDING = 10.0f;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 work_pos = viewport->WorkPos;
    ImVec2 work_size = viewport->WorkSize;
    ImVec2 window_pos, window_pos_pivot;

    switch (position) {
        case Position::TOP_LEFT:
            window_pos.x = work_pos.x + PADDING;
            window_pos.y = work_pos.y + PADDING;
            window_pos_pivot.x = 0.0f;
            window_pos_pivot.y = 0.0f;
            break;
        case Position::TOP_RIGHT:
            window_pos.x = work_pos.x + work_size.x - PADDING;
            window_pos.y = work_pos.y + PADDING;
            window_pos_pivot.x = 1.0f;
            window_pos_pivot.y = 0.0f;
            break;
        case Position::BOTTOM_LEFT:
            window_pos.x = work_pos.x + PADDING;
            window_pos.y = work_pos.y + work_size.y - PADDING;
            window_pos_pivot.x = 0.0f;
            window_pos_pivot.y = 1.0f;
            break;
        case Position::BOTTOM_RIGHT:
            window_pos.x = work_pos.x + work_size.x - PADDING;
            window_pos.y = work_pos.y + work_size.y - PADDING;
            window_pos_pivot.x = 1.0f;
            window_pos_pivot.y = 1.0f;
            break;
    }

    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
    ImGui::SetNextWindowBgAlpha(0.7f);  // Transparent background

    if (!ImGui::Begin("Performance", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // FPS section
    if (showFPS) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "PERFORMANCE");
        ImGui::Separator();

        // Color-code FPS
        ImVec4 fpsColor;
        if (currentFPS >= 60.0f) {
            fpsColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // Green
        } else if (currentFPS >= 30.0f) {
            fpsColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // Yellow
        } else {
            fpsColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // Red
        }

        ImGui::Text("FPS: ");
        ImGui::SameLine();
        ImGui::TextColored(fpsColor, "%.1f", currentFPS);

        ImGui::Text("Avg: %.1f", averageFPS);
        ImGui::Text("Min: %.1f", minFPS);
        ImGui::Text("Max: %.1f", maxFPS);
        ImGui::Text("Frame: %.2f ms", frameTime * 1000.0f);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.6f, 1.0f), "CPU TIMINGS (ms)");
        ImGui::Text("Update: %.2f (Camera: %.2f)", renderer->getLastUpdateMs(), cameraUpdateMs_);
        ImGui::Text("Render: %.2f (Terrain: %.2f, WMO: %.2f, M2: %.2f)",
                    renderer->getLastRenderMs(),
                    renderer->getLastTerrainRenderMs(),
                    renderer->getLastWMORenderMs(),
                    renderer->getLastM2RenderMs());
        {
            const auto wmoQueries = renderer->collision().wmoQueryStats();
            const auto m2Queries = renderer->collision().m2QueryStats();
            ImGui::Text("Collision queries:");
            ImGui::Text("  WMO: %.2f ms (%u calls)", wmoQueries.milliseconds, wmoQueries.calls);
            ImGui::Text("  M2:  %.2f ms (%u calls)", m2Queries.milliseconds, m2Queries.calls);
        }

        // Frame time graph
        if (!frameTimeHistory.empty()) {
            std::vector<float> frameTimesMs;
            frameTimesMs.reserve(frameTimeHistory.size());
            for (float ft : frameTimeHistory) {
                frameTimesMs.push_back(ft * 1000.0f);  // Convert to ms
            }
            ImGui::PlotLines("##frametime", frameTimesMs.data(), static_cast<int>(frameTimesMs.size()),
                           0, nullptr, 0.0f, 33.33f, ImVec2(200, 40));
        }

        // FSR info
        if (renderer->getPostProcessPipeline()->isFSREnabled()) {
            ImGui::TextColored(colors::kGreen, "FSR 1.0: ON");
            auto* ctx = renderer->getVkContext();
            if (ctx) {
                auto ext = ctx->getSwapchainExtent();
                float sf = renderer->getPostProcessPipeline()->getFSRScaleFactor();
                uint32_t iw = static_cast<uint32_t>(ext.width * sf) & ~1u;
                uint32_t ih = static_cast<uint32_t>(ext.height * sf) & ~1u;
                ImGui::Text("  %ux%u -> %ux%u (%.0f%%)", iw, ih, ext.width, ext.height, sf * 100.0f);
            }
        }
        if (renderer->getPostProcessPipeline()->isFSR2Enabled()) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "FSR 3 Upscale: ON");
            ImGui::Text("  JitterSign=%.2f", renderer->getPostProcessPipeline()->getFSR2JitterSign());
            const bool fgEnabled = renderer->getPostProcessPipeline()->isAmdFsr3FramegenEnabled();
            const bool fgReady = renderer->getPostProcessPipeline()->isAmdFsr3FramegenRuntimeReady();
            const bool fgActive = renderer->getPostProcessPipeline()->isAmdFsr3FramegenRuntimeActive();
            const char* fgStatus = "Disabled";
            if (fgEnabled) {
                fgStatus = fgActive ? "Active" : (fgReady ? "Ready (waiting/fallback)" : "Unavailable");
            }
            ImGui::Text("  FSR3 FG: %s (%s)", fgStatus, renderer->getPostProcessPipeline()->getAmdFsr3FramegenRuntimePath());
            const std::string& fgErr = renderer->getPostProcessPipeline()->getAmdFsr3FramegenRuntimeError();
            if (!fgErr.empty()) {
                ImGui::TextWrapped("  FG Last Error: %s", fgErr.c_str());
            }
            ImGui::Text("  FG Dispatches: %zu", renderer->getPostProcessPipeline()->getAmdFsr3FramegenDispatchCount());
            ImGui::Text("  Upscale Dispatches: %zu", renderer->getPostProcessPipeline()->getAmdFsr3UpscaleDispatchCount());
            ImGui::Text("  FG Fallbacks: %zu", renderer->getPostProcessPipeline()->getAmdFsr3FallbackCount());
        }
        if (renderer->getPostProcessPipeline()->isFXAAEnabled()) {
            if (renderer->getPostProcessPipeline()->isFSR2Enabled()) {
                ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.8f, 1.0f), "FXAA: ON (FSR3+FXAA combined)");
            } else {
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.6f, 1.0f), "FXAA: ON");
            }
        }

        ImGui::Spacing();
        renderResources();
    }

    // Renderer stats
    if (showRenderer) {
        auto* terrainRenderer = renderer->getTerrainRenderer();
        if (terrainRenderer) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "RENDERING");
            ImGui::Separator();

            int totalChunks = terrainRenderer->getChunkCount();
            int rendered = terrainRenderer->getRenderedChunkCount();
            int culled = terrainRenderer->getCulledChunkCount();
            int triangles = terrainRenderer->getTriangleCount();

            ImGui::Text("Chunks: %d", totalChunks);
            ImGui::Text("Rendered: %d", rendered);
            ImGui::Text("Culled: %d", culled);

            if (totalChunks > 0) {
                float visiblePercent = (rendered * 100.0f) / totalChunks;
                ImGui::Text("Visible: %.1f%%", visiblePercent);
            }

            ImGui::Text("Triangles: %s",
                       triangles >= 1000000 ?
                       (std::to_string(triangles / 1000) + "K").c_str() :
                       std::to_string(triangles).c_str());

            ImGui::Spacing();
        }
    }

    // Terrain streaming info
    if (showTerrain) {
        auto* terrainManager = renderer->getTerrainManager();
        if (terrainManager) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "TERRAIN");
            ImGui::Separator();

            ImGui::Text("Loaded tiles: %d", terrainManager->getLoadedTileCount());

            auto currentTile = terrainManager->getCurrentTile();
            ImGui::Text("Current tile: [%d,%d]", currentTile.x, currentTile.y);

            ImGui::Spacing();
        }

        // Water info
        auto* waterRenderer = renderer->getWaterRenderer();
        if (waterRenderer) {
            const auto& liquid = renderer->liquidScene();
            ImGui::TextColored(ImVec4(0.2f, 0.5f, 1.0f, 1.0f), "WATER");
            ImGui::Separator();

            ImGui::Text("Surfaces: %d (%d drawn)", static_cast<int>(liquid.surfaceCount()),
                        waterRenderer->getDrawnSurfaceCount());
            ImGui::Text("Bodies: %d", static_cast<int>(liquid.bodies().size()));
            ImGui::Text("Enabled: %s", waterRenderer->isEnabled() ? "YES" : "NO");

            ImGui::Spacing();
        }
    }

    // Skybox info
    if (showTerrain) {
        auto* skybox = renderer->getSkybox();
        if (skybox) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "SKY");
            ImGui::Separator();

            float time = skybox->getTimeOfDay();
            int hours = static_cast<int>(time);
            int minutes = static_cast<int>((time - hours) * 60);

            ImGui::Text("Time: %02d:%02d", hours, minutes);
            ImGui::Text("Auto: %s", skybox->isTimeProgressionEnabled() ? "YES" : "NO");

            // The sun's and the moon's discs: drawn, or why not.
            if (auto* bodies = renderer->getSkyBodies()) {
                const auto line = [&](const char* name, SkyBodies::Body body) {
                    const auto& draw = bodies->lastDiscDraw(body);
                    ImGui::Text("%s: %s", name, draw.drawn ? "drawn" : draw.skipped);
                };
                line("Sun", SkyBodies::Body::Sun);
                line("Moon", SkyBodies::Body::Moon);
            }

            // The setting selects point stars or the sky model's texture;
            // it does not switch off the night sky's stars.
            auto* starField = renderer->getStarField();
            if (starField) {
                if (renderer->areSharpStars()) {
                    ImGui::Text("Sharp stars: ON (%d points)", starField->getStarCount());
                } else {
                    ImGui::TextUnformatted("Sharp stars: OFF (sky texture)");
                }
            }

            // Cloud info
            auto* clouds = renderer->getClouds();
            if (clouds) {
                ImGui::Text("Clouds: %s (%.0f%%)",
                           clouds->isEnabled() ? "ON" : "OFF",
                           clouds->getDensity() * 100.0f);
            }

            // Lens flare info
            auto* lensFlare = renderer->getLensFlare();
            if (lensFlare) {
                ImGui::Text("Lens Flare: %s (%.0f%%)",
                           lensFlare->isEnabled() ? "ON" : "OFF",
                           lensFlare->getIntensity() * 100.0f);
            }

            ImGui::Spacing();
        }
    }

    // Weather info
    if (showRenderer) {
        {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "WEATHER");
            ImGui::Separator();

            const auto& target = renderer->weatherTarget();
            const std::string kind(pipeline::weatherKindName(target.kind));
            ImGui::Text("Type: %s", kind.c_str());
            if (renderer->weatherMix().any()) {
                ImGui::Text("Particles: %u", renderer->precipitationCount());
                ImGui::Text("Intensity: %.0f%%", renderer->weatherMix().strength() * 100.0f);
                ImGui::Text("Open to sky: %.0f%%", renderer->skyExposure() * 100.0f);
            }

            auto* lightning = renderer->getLightning();
            if (lightning && lightning->isEnabled()) {
                ImGui::Text("Lightning: active (%.0f%%)", lightning->getIntensity() * 100.0f);
            }

            ImGui::Spacing();
        }
    }

    // Fog info
    if (showRenderer) {
        auto* terrainRenderer = renderer->getTerrainRenderer();
        if (terrainRenderer) {
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 0.9f, 1.0f), "FOG");
            ImGui::Separator();

            ImGui::Text("Distance fog: %s", terrainRenderer->isFogEnabled() ? "ON" : "OFF");

            ImGui::Spacing();
        }
    }

    // Character info
    if (showRenderer) {
        auto* charRenderer = renderer->getCharacterRenderer();
        if (charRenderer) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "CHARACTERS");
            ImGui::Separator();

            ImGui::Text("Instances: %zu", charRenderer->getInstanceCount());

            ImGui::Spacing();
        }
    }

    // WMO building info
    if (showRenderer) {
        auto* wmoRenderer = renderer->getWMORenderer();
        if (wmoRenderer) {
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.6f, 1.0f), "WMO BUILDINGS");
            ImGui::Separator();

            ImGui::Text("Models: %u", wmoRenderer->getModelCount());
            ImGui::Text("Instances: %u", wmoRenderer->getInstanceCount());
            ImGui::Text("Triangles: %u", wmoRenderer->getTotalTriangleCount());
            ImGui::Text("Draw Calls: %u", wmoRenderer->getDrawCallCount());
            ImGui::Text("Dist Culled: %u groups", wmoRenderer->getDistanceCulledGroups());
            if (wmoRenderer->isOcclusionCullingEnabled()) {
                ImGui::Text("Occl Culled: %u groups", wmoRenderer->getOcclusionCulledGroups());
            }
            if (wmoRenderer->isPortalCullingEnabled()) {
                ImGui::Text("Portal Culled: %u groups", wmoRenderer->getPortalCulledGroups());
            }

            ImGui::Spacing();
        }
    }

    // Zone info
    {
        const std::string& zoneName = zoneName_;
        if (!zoneName.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "ZONE");
            ImGui::Separator();
            ImGui::Text("%s", zoneName.c_str());
            ImGui::Spacing();
        }
    }

    // Camera info
    if (showCamera && camera) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "CAMERA");
        ImGui::Separator();

        glm::vec3 pos = camera->getPosition();
        ImGui::Text("Pos: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);

        glm::vec3 forward = camera->getForward();
        ImGui::Text("Dir: %.2f, %.2f, %.2f", forward.x, forward.y, forward.z);

        ImGui::Spacing();
    }

    // Controls help
    if (showControls) {
        ImGui::TextColored(kTitle, "CONTROLS");
        ImGui::Separator();

        ImGui::TextColored(kSectionHeader, "Movement");
        ImGui::TextColored(kHelpText, "WASD: Move/Strafe");
        ImGui::TextColored(kHelpText, "Q/E: Strafe left/right");
        ImGui::TextColored(kHelpText, "Space: Jump");
        ImGui::TextColored(kHelpText, "X: Sit/Stand");
        ImGui::TextColored(kHelpText, "~: Auto-run");
        ImGui::TextColored(kHelpText, "Z: Sheathe weapons");

        ImGui::Spacing();
        ImGui::TextColored(kSectionHeader, "UI Panels");
        ImGui::TextColored(kHelpText, "B: Bags/Inventory");
        ImGui::TextColored(kHelpText, "C: Character sheet");
        ImGui::TextColored(kHelpText, "L: Quest log");
        ImGui::TextColored(kHelpText, "N: Talents");
        ImGui::TextColored(kHelpText, "P: Spellbook");
        ImGui::TextColored(kHelpText, "M: World map");

        ImGui::Spacing();
        ImGui::TextColored(kSectionHeader, "Combat & Chat");
        ImGui::TextColored(kHelpText, "1-0,-,=: Action bar");
        ImGui::TextColored(kHelpText, "Tab: Target cycle");
        ImGui::TextColored(kHelpText, "Enter: Chat");
        ImGui::TextColored(kHelpText, "/: Chat command");

        ImGui::Spacing();
        ImGui::TextColored(kSectionHeader, "Debug");
        ImGui::TextColored(kHelpText, "%s: Toggle this HUD",
                           core::clientShortcutLabel(core::ClientShortcut::TogglePerformanceHud));
        ImGui::TextColored(kHelpText, "F4: Toggle shadows");
        ImGui::TextColored(kHelpText, "F7: Level-up FX");
        ImGui::TextColored(kHelpText, "Esc: Settings/Close");
    }

    ImGui::End();
}

void PerformanceHUD::renderResources() const {
    if (!resources_) return;
    using core::ResourceSeries;
    const core::ResourceSample* latest = resources_->latest();

    const size_t seconds = resources_->series(ResourceSeries::Threads).size();
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "RESOURCES (last %zu:%02zu)",
                       seconds / 60, seconds % 60);
    ImGui::Separator();
    if (!latest) {
        ImGui::TextColored(kHelpText, "First sample in a second");
        ImGui::Spacing();
        return;
    }

    const auto ms = [](float v) { return whole(v) + " ms"; };
    plotSeries("Latency", resources_->series(ResourceSeries::LatencyMs), ms);
    plotSeries("Working set", resources_->series(ResourceSeries::WorkingSetMB), megabytes);
    plotSeries("Private commit", resources_->series(ResourceSeries::PrivateMB), megabytes);
    plotSeries("System commit used", resources_->series(ResourceSeries::SystemCommitUsedMB), megabytes);
    const auto& process = latest->process;
    if (process.systemCommitUsedBytes && process.systemCommitLimitBytes) {
        const uint64_t used = *process.systemCommitUsedBytes;
        const uint64_t limit = *process.systemCommitLimitBytes;
        ImGui::Text("  of %s limit, %s free", core::formatBytes(limit).c_str(),
                    core::formatBytes(limit - std::min(used, limit)).c_str());
    }
    if (process.availablePhysicalBytes && process.totalPhysicalBytes) {
        ImGui::Text("Physical free: %s of %s", core::formatBytes(*process.availablePhysicalBytes).c_str(),
                    core::formatBytes(*process.totalPhysicalBytes).c_str());
    }
    plotSeries("Threads", resources_->series(ResourceSeries::Threads), whole);

    if (latest->videoMemory) {
        const auto& video = *latest->videoMemory;
        plotSeries("Video memory", resources_->series(ResourceSeries::VideoMemoryMB), megabytes);
        ImGui::Text("  of %s budget", core::formatBytes(video.budgetBytes).c_str());
        if (!video.parts.empty() &&
            ImGui::BeginTable("##videoMemory", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            for (const auto& part : video.parts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(part.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(core::formatBytes(part.value).c_str());
            }
            ImGui::EndTable();
        }
    }

    if (!latest->caches.empty() &&
        ImGui::BeginTable("##caches", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Cache");
        ImGui::TableSetupColumn("Entries");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Budget");
        ImGui::TableHeadersRow();
        for (const auto& cache : latest->caches) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cache.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", cache.stats.entries);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(core::formatBytes(cache.stats.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cache.stats.budgetBytes > 0
                                       ? core::formatBytes(cache.stats.budgetBytes).c_str()
                                       : "-");
        }
        ImGui::EndTable();
    }
    if (!latest->counts.empty() &&
        ImGui::BeginTable("##counts", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        for (const auto& count : latest->counts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(count.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(count.value));
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
}

} // namespace rendering
} // namespace wowee
