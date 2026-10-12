#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wowee::game {
enum class ProtocolResult { Unhandled, Handled, Error };
struct KnownTitleUpdate {
    uint8_t operation; // 1 add, 2 remove, 3 replace
    std::vector<uint16_t> bits;
};
struct ProtocolEvents {
    // Invoked synchronously by world-packet dispatch, never by preparation or Warden workers.
    std::function<void(const KnownTitleUpdate&)> knownTitles{};
    std::function<void(uint64_t)> money{};
};
struct ProtocolSession {
    std::string host;
    uint16_t port;
    uint32_t build;
    ProtocolEvents events{};
};
using ProtocolSend = std::function<void(uint16_t, const std::vector<uint8_t>&)>;

// Server-specific packet meanings belong in adapters. The world socket remains
// responsible for encryption and framing, and owns the session credentials.
class ServerProtocolAdapter {
public:
    virtual ~ServerProtocolAdapter() = default;
    virtual ProtocolResult packet(uint16_t opcode, std::span<const uint8_t> body,
                                  const ProtocolSend& send) = 0;
    virtual void warden(std::span<const uint8_t>) {}
    // Deferred hint, consumed on the main thread after pending Warden replies.
    virtual bool takeCharacterListRetryRequest() { return false; }
    // An earlier ordinary query can overlap server checks; it never claims readiness.
    virtual bool takeEarlyCharacterListRetryRequest() { return false; }
    virtual ProtocolResult memory(const std::string&, uint32_t, uint8_t, uint8_t*) {
        return ProtocolResult::Unhandled;
    }
    virtual std::string error() const = 0;
};
bool isServerProtocolDiagnosticOpcode(uint16_t opcode);
std::unique_ptr<ServerProtocolAdapter> makeServerProtocol(
    const std::string& name, const ProtocolSession& session);

// EZWoW's registered 0x41D handler reads the offset at CDataStore+6.
// CDataStore prepends a two-byte opcode; +6 skips that and the u32 command.
// World packet bodies start at the command. No downloaded code is executed.
struct EzWowPayload {
    uint32_t command;
    std::span<const uint8_t> arguments;
    std::span<const uint8_t> code;
    std::span<const uint8_t> trailing;
};
uint64_t parseEzWowMoney(std::span<const uint8_t> body);
KnownTitleUpdate parseEzWowKnownTitles(std::span<const uint8_t> body);
EzWowPayload parseEzWowPayload(std::span<const uint8_t> body);
struct EzWowPatch {
    std::string name;
    uint32_t size;
    uint32_t flags;
    std::array<uint8_t,16> checksum;
    std::array<uint8_t,16> fastChecksum;
};
std::vector<EzWowPatch> parseEzWowPatches(std::span<const uint8_t> body);


// Optional, bounded, per-session record of extension traffic only. No auth
// packets, SRP keys, Warden module keys or seeds are passed to this recorder.
class ServerProtocolTrace {
public:
    explicit ServerProtocolTrace(const std::filesystem::path& directory);
    ~ServerProtocolTrace();
    void record(uint32_t channel, uint16_t opcode, std::span<const uint8_t> body);
    const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
