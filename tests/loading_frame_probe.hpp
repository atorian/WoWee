#pragma once

#include "core/event_bus.hpp"
#include "core/frame_observer.hpp"
#include "core/frame_plan.hpp"
#include <chrono>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace wowee::core {
class Application;

/// Native integration fixture: watches the real client's frames through the
/// offline world fixture (WOWEE_MULGORE_PROBE) and drives the scenario named
/// by WOWEE_LOADING_TEST. Only wowee_loading_test_client builds one; the
/// ordinary client has no observer.
class LoadingFrameProbe final : public FrameObserver {
public:
    LoadingFrameProbe(Application& app, EventBus& events, std::string action);
    void frameBegan() override;
    void streamingDone(const FramePlan& plan) override;
    void interfaceStarting(const FramePlan& plan) override;
    void loopEnded() override;
private:
    using Clock = std::chrono::steady_clock;
    void pass(const char* detail);
    void fail(const char* detail);
    Application& app_;
    EventBus& events_;
    std::vector<EventBus::Subscription> subscriptions_;
    std::string action_;
    std::string waitStage_;
    uint64_t frame_ = 0, actionFrame_ = 0;
    int loginFrames_ = 0;
    int enteredFrames_ = 0;
    glm::ivec2 targetSize_{1000, 700};
    bool fired_ = false, done_ = false, installed_ = false;
    bool posted_ = false, sentEvents_ = false, luaVerified_ = false;
    bool keyHeld_ = false, keyReleased_ = false, inWorld_ = false;
    bool entrySpawnQueued_ = false;
    Clock::time_point started_ = Clock::now(), actionAt_{}, worldAt_{};
    glm::vec3 worldPosition_{0};
};
}
