#include "rendering/renderer.hpp"
#include "rendering/frame_scene.hpp"
#include "rendering/frame_sink.hpp"
#include "rendering/screen_capture.hpp"
#include "rendering/shelter_map.hpp"
#include "rendering/sky_bodies.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "core/logger.hpp"
#include <stb_image_write.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace wowee::rendering {

void Renderer::setMapJobToken(core::JobGroup::Token token) {
    mapJobToken_ = std::move(token);
    ++worldEpoch_;
    publishedExposure_ = shelter_ ? 0.0f : 1.0f;
    publishedSkyView_ = 1;
    publishedSun_ = publishedMoon_ = 0;
}

uint64_t Renderer::epochForGpuFrame(uint64_t frame) const {
    if (!frame) return 0;
    if (frame == recordingGpuFrame_) return frameInputs_.worldEpoch;
    for (const auto& [number, epoch] : submittedWorlds_) if (number == frame) return epoch;
    return 0;
}

void Renderer::publishFrameResults() {
    auto& result = recordingResult_;
    if (vkCtx) {
        if (vkCtx->frameNumber() > recordingGpuFrame_) {
            result.submittedFrame = recordingGpuFrame_;
            submittedWorlds_.emplace_back(recordingGpuFrame_, frameInputs_.worldEpoch);
            if (submittedWorlds_.size() > 8) submittedWorlds_.erase(submittedWorlds_.begin());
        }
        for (const auto& part : vkCtx->cpuStages().entries())
            result.cpu.push_back({std::string(part.group), std::string(part.name), part.milliseconds});
        result.gpuTimingSerial = vkCtx->gpuTimingsSerial();
        result.gpuTimingFrame = vkCtx->gpuTimingsFrame();
        for (const auto& [label, ms] : vkCtx->gpuTimings()) result.gpu.emplace_back(label, ms);
        const auto extent = vkCtx->getSwapchainExtent();
        result.statistics.width = extent.width;
        result.statistics.height = extent.height;
    }
    if (shelter_) {
        const auto frame = shelter_->answerFrame();
        const auto epoch = epochForGpuFrame(frame);
        result.exposure = {frame, epoch, shelter_->exposure()};
        result.skyView = {frame, epoch, shelter_->skyView()};
    }
    if (const auto* bodies = skySystem ? skySystem->getBodies() : nullptr) {
        const auto sun = SkyBodies::Body::Sun, moon = SkyBodies::Body::Moon;
        result.sun = {bodies->visibilityFrame(sun), epochForGpuFrame(bodies->visibilityFrame(sun)), bodies->measuredVisibility(sun)};
        result.moon = {bodies->visibilityFrame(moon), epochForGpuFrame(bodies->visibilityFrame(moon)), bodies->measuredVisibility(moon)};
        const auto& sunDraw = bodies->lastDiscDraw(sun);
        const auto& moonDraw = bodies->lastDiscDraw(moon);
        result.statistics.sunDisc = sunDraw.drawn ? "drawn" : sunDraw.skipped;
        result.statistics.moonDisc = moonDraw.drawn ? "drawn" : moonDraw.skipped;
    }
    if (worldDrawnThisFrame_) {
        result.statistics.renderMs = lastRenderMs;
        result.statistics.terrainMs = lastTerrainRenderMs;
        result.statistics.wmoMs = lastWMORenderMs;
        result.statistics.m2Ms = lastM2RenderMs;
    }
    if (terrainRenderer) {
        result.statistics.terrainDrawn = terrainRenderer->getRenderedChunkCount();
        result.statistics.terrainCulled = terrainRenderer->getCulledChunkCount();
        result.statistics.terrainTriangles = terrainRenderer->getRenderedTriangleCount();
    }
    if (m2Renderer) {
        result.statistics.m2Draws = m2Renderer->getDrawCallCount();
    }
    if (wmoRenderer) {
        result.statistics.wmoDraws = wmoRenderer->getDrawCallCount();
        result.statistics.wmoShadows = wmoRenderer->getShadowDrawCallCount();
    }
    if (frameInputs_.scene && frameInputs_.scene->buildings) {
        const auto& view = frameInputs_.scene->buildings->view;
        result.statistics.wmoDistanceCulled = view.distanceCulled;
        result.statistics.wmoPortalCulled = view.portalCulled;
    }
    if (rtScene_) {
        const auto stats = rtScene_->stats();
        result.statistics.rtMeshes = stats.meshes;
        result.statistics.rtInstances = stats.instances;
        result.statistics.rtTriangles = stats.triangles;
        result.statistics.rtGpuBytes = stats.gpuBytes;
        result.statistics.rtSourceBytes = stats.sourceBytes;
    }
    if (waterRenderer) result.statistics.waterDrawn = waterRenderer->getDrawnSurfaceCount();
    if (postProcessPipeline_) {
        auto& stats = result.statistics;
        stats.framegenActive = postProcessPipeline_->isAmdFsr3FramegenRuntimeActive();
        stats.framegenReady = postProcessPipeline_->isAmdFsr3FramegenRuntimeReady();
        stats.framegenPath = postProcessPipeline_->getAmdFsr3FramegenRuntimePath();
        stats.framegenError = postProcessPipeline_->getAmdFsr3FramegenRuntimeError();
        stats.framegenDispatches = postProcessPipeline_->getAmdFsr3FramegenDispatchCount();
        stats.upscaleDispatches = postProcessPipeline_->getAmdFsr3UpscaleDispatchCount();
        stats.framegenFallbacks = postProcessPipeline_->getAmdFsr3FallbackCount();
    }
    if (!result.submittedFrame) {
        if (frameInputs_.capture.pts) result.captureDrops.push_back(frameInputs_.capture.generation);
        for (const auto& path : frameInputs_.screenshots) result.screenshots.emplace_back(path, false);
    }
    results_.publish(std::move(result));
}

std::vector<FrameResults> Renderer::takeFrameResults() {
    auto results = results_.take();
    for (auto& result : results) {
        if (result.recording) publishedStatistics_ = result.statistics;
        auto accept = [&](const VisibilityAnswer& answer, float& value) {
            if (answer.gpuFrame && answer.worldEpoch == worldEpoch_) value = answer.value;
        };
        accept(result.exposure, publishedExposure_);
        accept(result.skyView, publishedSkyView_);
        accept(result.sun, publishedSun_);
        accept(result.moon, publishedMoon_);
        for (const auto generation : result.captureDrops)
            if (frameSink_ && generation == captureGeneration_) frameSink_->frameDropped();
        for (auto& image : result.images) {
            if (!image.screenshot.empty()) {
                bool saved = false;
                try {
                    const auto path = std::filesystem::path(image.screenshot);
                    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
                    std::vector<uint8_t> rgba(static_cast<size_t>(image.width) * image.height * 4);
                    for (uint32_t y = 0; y < image.height; ++y) {
                        auto* row = rgba.data() + static_cast<size_t>(y) * image.width * 4;
                        std::memcpy(row, image.pixels.get() + static_cast<size_t>(y) * image.stride, image.width * 4);
                        for (uint32_t x = 0; x < image.width; ++x) std::swap(row[x * 4], row[x * 4 + 2]);
                    }
                    saved = stbi_write_png(image.screenshot.c_str(), image.width, image.height,
                                            4, rgba.data(), image.width * 4) != 0;
                } catch (const std::exception& e) {
                    LOG_WARNING("Screenshot ", image.screenshot, " could not be saved: ", e.what());
                }
                result.screenshots.emplace_back(image.screenshot, saved);
            } else if (frameSink_ && image.generation == captureGeneration_) {
                auto pixels = image.pixels;
                frameSink_->submit({.bgra = pixels.get(), .stride = image.stride, .pts = image.pts,
                    .release = [pixels = std::move(pixels)]() mutable { pixels.reset(); }});
            }
            image.pixels.reset();
        }
        for (const auto& [path, saved] : result.screenshots) {
            if (saved) LOG_INFO("Screenshot saved: ", path);
            else LOG_WARNING("Screenshot ", path, " failed; no image was saved");
            screenshotResults_.emplace_back(path, saved);
        }
    }
    return results;
}

std::vector<std::pair<std::string, bool>> Renderer::takeScreenshotResults() {
    std::vector<std::pair<std::string, bool>> ready;
    ready.swap(screenshotResults_);
    return ready;
}

bool Renderer::captureScreenshot(const std::string& path) {
    if (!vkCtx || path.empty()) return false;
    if (screenshotRequests_.size() >= 8) {
        LOG_WARNING("Screenshot ", path, " refused: eight requests are already pending");
        return false;
    }
    screenshotRequests_.push_back(path);
    return true;
}

FrameCaptureRequest Renderer::extractCaptureRequest() {
    FrameCaptureRequest request;
    if (!frameSink_ || !screenCapture_) return request;
    request.pool = screenCapture_;
    request.generation = captureGeneration_;
    request.width = captureWidth_; request.height = captureHeight_;
    request.elapsed = frameSink_->elapsedSeconds();
    int64_t pts = 0;
    if (frameSink_->frameDue(&pts)) {
        request.pts = pts;
        frameSink_->frameTaken(pts);
    }
    return request;
}

bool Renderer::beginFrameCapture(FrameSink* sink, uint32_t width, uint32_t height) {
    drainFrames();
    if (!vkCtx || !sink || frameSink_) return false;
    auto capture = std::make_shared<ScreenCapture>();
    if (!capture->initialize(vkCtx, width, height)) return false;
    screenCapture_ = std::move(capture);
    frameSink_ = sink;
    captureWidth_ = width; captureHeight_ = height;
    ++captureGeneration_;
    return true;
}

void Renderer::queueCapturedImage(const std::shared_ptr<ScreenCapture>& pool, uint32_t slot,
        uint32_t width, uint32_t height, uint64_t generation, const std::string& screenshot) {
    const auto ready = pool->collect(slot);
    if (!ready.valid) return;
    auto pixels = std::shared_ptr<const uint8_t>(ready.bgra, [pool, buffer = ready.buffer](const uint8_t*) { pool->release(buffer); });
    recordingResult_.images.push_back({std::move(pixels), width, height, ready.stride,
                                      ready.pts, generation, ready.frame, screenshot});
}

void Renderer::collectScreenshots(uint32_t slot) {
    std::erase_if(pendingScreenshots_, [&](const PendingScreenshot& shot) {
        if (shot.slot != slot) return false;
        queueCapturedImage(shot.pool, slot, shot.width, shot.height, 0, shot.path);
        return true;
    });
}

void Renderer::collectRecordedFrame() {
    const auto slot = vkCtx->getCurrentFrame();
    const auto& request = frameInputs_.capture;
    if (request.pool) queueCapturedImage(request.pool, slot, request.width, request.height, request.generation);
    collectScreenshots(slot);
}

void Renderer::flushFrameCapture() {
    drainFrames();
    if (!vkCtx || vkCtx->waitForSubmittedFrames("flush frame readbacks") != VK_SUCCESS) return;
    recordingResult_ = FrameResults();
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        if (screenCapture_) queueCapturedImage(screenCapture_, slot, captureWidth_, captureHeight_, captureGeneration_);
        collectScreenshots(slot);
    }
    std::sort(recordingResult_.images.begin(), recordingResult_.images.end(),
              [](const auto& a, const auto& b) { return a.gpuFrame < b.gpuFrame; });
    results_.publish(std::move(recordingResult_));
    (void)takeFrameResults();
}

void Renderer::endFrameCapture() {
    drainFrames();
    frameSink_ = nullptr;
    ++captureGeneration_;
    screenCapture_.reset();
}

void Renderer::recordScreenCapture() {
    if (currentCmd == VK_NULL_HANDLE) return;
    const auto& images = vkCtx->getSwapchainImages();
    if (currentImageIndex >= images.size()) return;
    const auto extent = vkCtx->getSwapchainExtent();
    const auto slot = vkCtx->getCurrentFrame();
    const auto& request = frameInputs_.capture;
    if (request.pool && request.pts && !request.pool->record(currentCmd, slot, images[currentImageIndex], extent, *request.pts))
        recordingResult_.captureDrops.push_back(request.generation);
    for (const auto& path : frameInputs_.screenshots) {
        auto pool = std::make_shared<ScreenCapture>();
        if (!pool->initialize(vkCtx, extent.width, extent.height, false) ||
            !pool->record(currentCmd, slot, images[currentImageIndex], extent, 0)) {
            recordingResult_.screenshots.emplace_back(path, false);
            continue;
        }
        pendingScreenshots_.push_back({std::move(pool), slot, extent.width, extent.height, path});
    }
    const auto& overlays = vkCtx->getOverlayFramebuffers();
    if (request.pool && currentImageIndex < overlays.size())
        request.pool->drawIndicator(currentCmd, overlays[currentImageIndex], extent, request.elapsed);
}

} // namespace wowee::rendering
