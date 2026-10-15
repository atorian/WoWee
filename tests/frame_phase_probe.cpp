#include "core/frame_phase.hpp"

#include <iostream>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
#endif
    using namespace wowee::core;
    if (argc > 1 && std::string_view(argv[1]) == "valid") {
        auto phase = Phase::Transitions;
        {
            ScopedPhase nested(phase, Phase::Network);
        }
        return requirePhase(phase, Phase::Transitions, "world entry") ? 0 : 1;
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
        if (requirePhase(Phase::Network, Phase::Transitions, "world entry")) return 2;
        if (requirePhase(Phase::Render, Phase::Transitions, "world entry")) return 2;
        if (requirePhase(Phase::Network, Phase::Transitions, "world logout")) return 2;
    }
    std::cout << "client continued\n";
    return 0;
}
