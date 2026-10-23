#pragma once
#include "game/server_protocol.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wowee::game {
// Profiles are local configuration, keyed by the AUTH endpoint (not a realm IP).
// Every connection owns a new process. A missing profile means stock WoW.
std::string serverExtensionKey(std::string host, uint16_t port);
// Where an endpoint's profile is: the folder named by WOWEE_SERVER_EXTENSIONS_DIR,
// or else server_extensions/ in the data folders the client reads its assets
// from (WOW_DATA_PATH, then Data/ in the working directory), the first that
// holds one. Not Data/ alone: a client started with WOW_DATA_PATH and no Data/
// beside it read its assets and never its realm's profile. `root` is empty
// when no folder holds one; `searched` names every folder looked in.
struct ServerExtensionLocation {
    std::filesystem::path root;
    std::vector<std::filesystem::path> searched;
};
ServerExtensionLocation locateServerExtension(const std::string& host, uint16_t port);
class ServerExtension {
public:
    ServerExtension();
    ~ServerExtension();
    ServerExtension(const ServerExtension&) = delete;
    ServerExtension& operator=(const ServerExtension&) = delete;
    // Reads root/<serverExtensionKey>/profile.json. Verifies/downloads declared
    // artifacts before spawning an adapter host. Fails closed on a bad profile.
    bool begin(const std::filesystem::path& root, const std::string& host,
               uint16_t port, uint32_t build, const std::filesystem::path& defaultHost,
               const ProtocolEvents& events = {});
    void reset();
    enum class ReadResult { Unhandled, Handled, Error };
    ReadResult readMemory(const std::string& module, uint32_t address,
                          uint8_t length, uint8_t* output);
    ProtocolResult packet(uint16_t opcode, const std::vector<uint8_t>& body, const ProtocolSend& send);
    void observeWarden(const std::vector<uint8_t>& plaintext);
    bool takeCharacterListRetryRequest();
    bool takeEarlyCharacterListRetryRequest();
    bool active() const;
    const std::string& error() const;
    const std::string& clientPath() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
