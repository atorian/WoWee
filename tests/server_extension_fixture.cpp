#include "game/server_extension_api.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
namespace {
std::string host;
unsigned reads = 0;
int WOWEE_EXTENSION_CALL start(const WoweeExtensionSession* session) {
    host = session->server_host; reads = 0;
    std::puts("Fixture diagnostics must not corrupt the IPC channel");
    return WOWEE_EXTENSION_HANDLED;
}
void WOWEE_EXTENSION_CALL stop() { host.clear(); }
int WOWEE_EXTENSION_CALL read(const char* module, uint32_t address, uint32_t count, uint8_t* out) {
    if (address == 0xDEAD) { std::this_thread::sleep_for(std::chrono::seconds(15)); return WOWEE_EXTENSION_UNHANDLED; }
    if (address == 0xD1ED) std::_Exit(21);
    if (std::string(module) != "fixture.dll" || address != 0x1000 || count != 4) return WOWEE_EXTENSION_UNHANDLED;
    out[0] = static_cast<uint8_t>(++reads); out[1] = host.empty() ? 0 : host.front(); out[2] = 0x71; out[3] = 0x93;
    return WOWEE_EXTENSION_HANDLED;
}
const char* WOWEE_EXTENSION_CALL error() { return "fixture failure"; }
const WoweeExtensionApi api{sizeof(WoweeExtensionApi), WOWEE_EXTENSION_ABI, start, stop, read, error};
}
extern "C" WOWEE_EXTENSION_EXPORT const WoweeExtensionApi* WOWEE_EXTENSION_CALL wowee_extension_api(uint32_t abi) {
    return abi == WOWEE_EXTENSION_ABI ? &api : nullptr;
}
