#include "core/screen_recording.hpp"

#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"

namespace wowee {
namespace core {

ScreenRecording::ScreenRecording(rendering::Renderer& renderer) : renderer_(renderer) {}

ScreenRecording::~ScreenRecording() {
    if (recorder_) stop();
}

bool ScreenRecording::start(const std::string& path, std::string& error) {
    if (recorder_ && recorder_->isRecording()) {
        error = "already recording";
        return false;
    }
    auto* vkCtx = renderer_.getVkContext();
    if (!vkCtx) {
        error = "there is no renderer to record from";
        return false;
    }
    if (!ScreenRecorder::compiledIn()) {
        error = "this build was made without FFmpeg 5.1 or later, which recording needs";
        return false;
    }
    const VkExtent2D extent = vkCtx->getSwapchainExtent();
    const RecordingSize size = recordingFrameSize(extent.width, extent.height);
    auto recorder = std::make_unique<ScreenRecorder>();
    if (!recorder->start(path, size, error)) return false;
    if (!renderer_.beginFrameCapture(recorder.get(), size.width, size.height)) {
        recorder->stop();
        error = "could not set up reading frames back from the GPU";
        return false;
    }
    recorder_ = std::move(recorder);
    failure_.clear();
    return true;
}

ScreenRecorder::Stats ScreenRecording::stop() {
    ScreenRecorder::Stats stats;
    if (!recorder_) return stats;
    // The frames still on the GPU are handed over first, so the file ends
    // where the recording did rather than two frames short; then the recorder
    // finishes, since its thread holds readback buffers until it is done; then
    // the buffers go.
    renderer_.flushFrameCapture();
    stats = recorder_->stop();
    renderer_.endFrameCapture();
    recorder_.reset();
    return stats;
}

bool ScreenRecording::isRecording() const {
    return recorder_ && recorder_->isRecording();
}

std::string ScreenRecording::takeFailure() {
    std::string failure;
    failure.swap(failure_);
    return failure;
}

void ScreenRecording::update() {
    // Given up on its own: finish what it has and say why, once.
    if (recorder_ && recorder_->failed()) {
        failure_ = recorder_->failure();
        stop();
    }
}

} // namespace core
} // namespace wowee
