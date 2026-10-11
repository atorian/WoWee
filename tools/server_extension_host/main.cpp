#include "game/server_extension_api.h"
#include "game/server_extension_wire.hpp"
#include <cstdio>
#include <iostream>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
using namespace wowee::game;
namespace wire = extension_wire;
namespace {
#ifdef _WIN32
HANDLE protocolOutput;
bool send(const std::vector<uint8_t>& bytes) {
    DWORD written = 0;
    return WriteFile(protocolOutput, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
           && written == bytes.size();
}
#else
int protocolOutput;
bool send(const std::vector<uint8_t>& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = write(protocolOutput, bytes.data() + offset, bytes.size() - offset);
        if (n <= 0) return false;
        offset += n;
    }
    return true;
}
#endif
bool reply(uint32_t operation, uint32_t status, const std::string& error,
           const std::vector<uint8_t>& data = {}) {
    std::vector<uint8_t> payload;
    wire::integer(payload, status); wire::string(payload, error.substr(0, 4096));
    payload.insert(payload.end(), data.begin(), data.end());
    return send(wire::frame(operation, payload));
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    // Keep plugin printf/iostream output out of the binary control channel.
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(),
                    &protocolOutput, 0, FALSE, DUPLICATE_SAME_ACCESS);
    _setmode(_fileno(stdin), _O_BINARY);
    _dup2(_fileno(stderr), _fileno(stdout));
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    int wideCount = 0;
    auto* wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    const auto module = wideArguments && wideCount == 2 ? LoadLibraryExW(wideArguments[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) : nullptr;
    if (wideArguments) LocalFree(wideArguments);
    const auto entry = module ? reinterpret_cast<WoweeExtensionEntry>(GetProcAddress(module, "wowee_extension_api")) : nullptr;
#else
    protocolOutput = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    const auto module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    const auto entry = module ? reinterpret_cast<WoweeExtensionEntry>(dlsym(module, "wowee_extension_api")) : nullptr;
#endif
    const auto* api = entry ? entry(WOWEE_EXTENSION_ABI) : nullptr;
    const bool valid = api && api->abi == WOWEE_EXTENSION_ABI && api->size >= sizeof(WoweeExtensionApi)
                    && api->start && api->stop && api->read_memory && api->last_error;
    bool started = false;
    std::string host, library, client, options;
    WoweeExtensionSession session{};
    try {
        for (;;) {
            std::vector<uint8_t> header(12);
            if (!std::cin.read(reinterpret_cast<char*>(header.data()), header.size())) break;
            wire::Reader h(header);
            if (h.integer() != wire::magic) break;
            const auto operation = h.integer(); const auto length = h.integer();
            if (length > wire::maxPayload) break;
            std::vector<uint8_t> payload(length);
            if (length && !std::cin.read(reinterpret_cast<char*>(payload.data()), length)) break;
            wire::Reader p(payload);
            if (operation == wire::Stop) { p.end(); break; }
            if (operation == wire::Start) {
                if (started) break;
                host = p.string(); const auto port = p.integer(); const auto build = p.integer();
                library = p.string(); client = p.string(); options = p.string(); p.end();
                if (!valid) {
                    reply(operation, 2, "Adapter could not be loaded, has the wrong architecture, or does not export ABI v1");
                    break;
                }
                session = {sizeof(WoweeExtensionSession), WOWEE_EXTENSION_ABI,
                    host.c_str(), port, build, library.c_str(), client.c_str(), options.c_str()};
                // Even a partially initialized adapter gets stop() on failure.
                started = true;
                if (api->start(&session) != WOWEE_EXTENSION_HANDLED) {
                    const char* why = api->last_error();
                    reply(operation, 2, why ? why : "Adapter initialization failed");
                    break;
                }
                if (!reply(operation, 1, "")) break;
            } else if (operation == wire::Read && started) {
                const auto name = p.string(); const auto address = p.integer(); const auto count = p.integer(); p.end();
                if (count > 255) break;
                std::vector<uint8_t> data(count);
                const auto result = api->read_memory(name.c_str(), address, count, data.data());
                if (result == WOWEE_EXTENSION_HANDLED) {
                    if (!reply(operation, 1, "", data)) break;
                } else if (result == WOWEE_EXTENSION_UNHANDLED) {
                    if (!reply(operation, 0, "")) break;
                } else {
                    const char* why = api->last_error();
                    if (!reply(operation, 2, why ? why : "Library runtime read failed")) break;
                }
            } else break;
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    if (started) api->stop();
#ifdef _WIN32
    if (module) FreeLibrary(module);
    CloseHandle(protocolOutput);
#else
    if (module) dlclose(module);
    close(protocolOutput);
#endif
    return 0;
}
