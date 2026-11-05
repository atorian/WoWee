#pragma once

#include <functional>
#include <memory>

namespace wowee::core {

class Application;
class FrameObserver;

/// What a program that is the client brings to it besides the client itself.
///
/// The ordinary client brings nothing. The native loading fixture
/// (wowee_loading_test_client) brings the observer that drives its scenario:
/// it is a main() of its own around the same client objects, so the ordinary
/// program carries no fixture code at all.
struct ClientProgram {
    /// Called once the client has initialized and before its first frame.
    std::function<std::unique_ptr<FrameObserver>(Application&)> observeFrames;
};

/// Everything a client program does: crash reporting, the working directory,
/// logging, then the Application's initialize, run and shutdown. Returns the
/// process exit code.
int runClient(const ClientProgram& program = {});

} // namespace wowee::core
