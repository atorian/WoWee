#pragma once

#include "core/frame_plan.hpp"

namespace wowee::core {

/// Something that watches the client's frames from outside: a native fixture
/// driving the real loop through a scenario, or a test harness. The ordinary
/// client has none.
///
/// Told at fixed points of ClientLoop::runFrame, in the phase that point is
/// in, so what it does there obeys that phase's rules - Lua only from
/// interfaceStarting, which is in Interface.
class FrameObserver {
public:
    virtual ~FrameObserver() = default;

    /// Input, before the platform is read: a frame has begun.
    virtual void frameBegan() {}
    /// Streaming, after the world load has advanced.
    virtual void streamingDone(const FramePlan& plan) { (void)plan; }
    /// Interface, before the interface hears anything this frame.
    virtual void interfaceStarting(const FramePlan& plan) { (void)plan; }
    /// The client's loop has ended; shutdown follows.
    virtual void loopEnded() {}
};

} // namespace wowee::core
