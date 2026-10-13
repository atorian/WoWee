#pragma once

#include "core/screen_recorder.hpp"

#include <memory>
#include <string>

namespace wowee {
namespace rendering { class Renderer; }

namespace core {

/// The screen being recorded to a video file: the client's encoder, with the
/// sound, fed by the frames the renderer reads back. Owned by the Application.
class ScreenRecording {
public:
    explicit ScreenRecording(rendering::Renderer& renderer);
    /// A recording in progress is finished, not abandoned: the file is only
    /// playable once its index is written.
    ~ScreenRecording();

    ScreenRecording(const ScreenRecording&) = delete;
    ScreenRecording& operator=(const ScreenRecording&) = delete;

    /// Start writing the screen, interface included, and what the client
    /// plays. False, with the reason in error, when it cannot.
    bool start(const std::string& path, std::string& error);
    /// Finish the file and close it.
    ScreenRecorder::Stats stop();
    [[nodiscard]] bool isRecording() const;
    /// Why a recording stopped by itself - the disk filled, the encoder gave
    /// up - once, for the interface to say; empty otherwise.
    std::string takeFailure();

    /// Once a frame: a recorder that has given up is finished, and why kept.
    void update();

private:
    rendering::Renderer& renderer_;
    std::unique_ptr<ScreenRecorder> recorder_;
    std::string failure_;
};

} // namespace core
} // namespace wowee
