// The ordinary client: the client and nothing else. Everything a client
// program does lives in runClient (src/core/client_main.cpp), which the native
// loading fixture's own main() calls with its frame observer.
#include "core/client_main.hpp"

// SDLActivity loads libwowee.so and calls SDL_main, the name this header gives
// main(). SDL2's SDL.h pulled it in; SDL3's does not, and without it the
// library exports only main and the activity has nothing to call.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    return wowee::core::runClient();
}
