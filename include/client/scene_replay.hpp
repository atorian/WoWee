#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string_view>

namespace wowee::rendering { struct FrameInputs; struct FrameResults; }
namespace wowee::core {
class Application;

/// Records producer inputs, not FrameInputs or GPU resources. The ordinary
/// client still performs world streaming, appearance loading and extraction.
class SceneReplay {
public:
    SceneReplay();
    ~SceneReplay();
    void initialize(Application&);
    void prepare(Application&);
    float deltaTime(float actual) const;
    void beforeGame(Application&);
    void placeCamera(Application&);
    void sampled(Application&, const rendering::FrameInputs&);
    void toggleCapture(Application&);
    void result(const rendering::FrameResults&);
    void stage(std::string_view, float milliseconds);
    void producerStarted();
    void extracted(uint64_t sequence, double queueWaitMs, size_t outstanding);
    void submitted(uint64_t sequence);
    void recordingStarted(uint64_t sequence);
    void recordingFinished(uint64_t sequence);
    void frameEnded(Application&);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
