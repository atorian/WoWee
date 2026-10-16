// wowee_loading_test_client: the real client, with the native loading
// fixture watching its frames (tests/loading_frame_probe.cpp). The same client
// objects as wowee.exe and this main instead of the client's; wowee.exe has no
// fixture code at all. Run it through tools/test_loading_frames.ps1.
#include "core/application.hpp"
#include "core/client_main.hpp"
#include "loading_frame_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    const char* action = std::getenv("WOWEE_LOADING_TEST");
    if (!action || !*action) {
        std::fprintf(stderr, "wowee_loading_test_client runs one loading scenario: set "
                             "WOWEE_LOADING_TEST (see tools/test_loading_frames.ps1)\n");
        return 2;
    }
    wowee::core::ClientProgram program;
    program.observeFrames = [scenario = std::string(action)](wowee::core::Application& app)
        -> std::unique_ptr<wowee::core::FrameObserver> {
        return std::make_unique<wowee::core::LoadingFrameProbe>(app, app.getEventBus(), scenario);
    };
    return wowee::core::runClient(program);
}
