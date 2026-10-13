#pragma once

#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include "game/server_extension.hpp"
#include <cstdint>
#include <functional>
#include <future>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
class WardenCrypto;
class WardenMemory;
class WardenModule;

class WardenHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit WardenHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API ---

    /** Reset all warden state (called on connect / disconnect). */
    void reset();
    bool beginSession(const std::string& host, uint16_t port, uint32_t build);
    std::string extensionError() const;
    bool takeCharacterListRetryRequest();
    bool takeEarlyCharacterListRetryRequest();
    bool handleProtocolPacket(network::Packet& packet);

    /** Initialize warden module manager (called once from GameHandler ctor). */

    /** Whether the server requires Warden (gates char enum / create). */
    [[nodiscard]] bool requiresWarden() const { return requiresWarden_; }
    void setRequiresWarden(bool v) { requiresWarden_ = v; }

    [[nodiscard]] bool wardenGateSeen() const { return wardenGateSeen_; }

    /** Increment packet-after-gate counter (called from handlePacket). */
    void notifyPacketAfterGate() { ++wardenPacketsAfterGate_; }

    [[nodiscard]] bool wardenCharEnumBlockedLogged() const { return wardenCharEnumBlockedLogged_; }
    void setWardenCharEnumBlockedLogged(bool v) { wardenCharEnumBlockedLogged_ = v; }

    /** Called from GameHandler::update() to drain async warden response + log gate timing. */
    void update(float deltaTime);

private:
    void handleWardenData(network::Packet& packet);
    bool loadWardenCRFile(const std::string& moduleHashHex);
    bool loadClientImage();
    bool readIntegrityMemory(const std::string& module, uint32_t address, uint8_t length, uint8_t* output);

    GameHandler& owner_;

    // --- Warden state ---
    bool requiresWarden_ = false;
    bool wardenGateSeen_ = false;
    float wardenGateElapsed_ = 0.0f;
    float wardenGateNextStatusLog_ = 2.0f;
    uint32_t wardenPacketsAfterGate_ = 0;
    bool wardenCharEnumBlockedLogged_ = false;
    std::unique_ptr<WardenCrypto> wardenCrypto_;
    std::unique_ptr<WardenMemory> wardenMemory_;
    ServerExtension serverExtension_;
    std::future<bool> extensionPreparation_;
    bool extensionPreparationPending_ = false;
    std::string extensionPreparationError_;
    std::deque<network::Packet> extensionWaitingPackets_;
    size_t extensionWaitingBytes_ = 0;

    // Warden module download state
    enum class WardenState {
        WAIT_MODULE_USE,     // Waiting for first SMSG (MODULE_USE)
        WAIT_MODULE_CACHE,   // Sent MODULE_MISSING, receiving module chunks
        WAIT_HASH_REQUEST,   // Module received, waiting for HASH_REQUEST
        WAIT_CHECKS,         // Hash sent, waiting for check requests
    };
    WardenState wardenState_ = WardenState::WAIT_MODULE_USE;
    std::vector<uint8_t> wardenModuleHash_;    // 16 bytes MD5
    std::vector<uint8_t> wardenModuleKey_;     // 16 bytes RC4
    uint32_t wardenModuleSize_ = 0;
    std::vector<uint8_t> wardenModuleData_;    // Downloaded module chunks
    bool wardenKnownModuleVerified_ = false; // Download MD5 matches a supported built-in module
    std::shared_ptr<WardenModule> wardenLoadedModule_; // Loaded Warden module

    // Pre-computed challenge/response entries from .cr file
    struct WardenCREntry {
        uint8_t seed[16];
        uint8_t reply[20];
        uint8_t clientKey[16];  // Encrypt key (client→server)
        uint8_t serverKey[16]; // Decrypt key (server→client)
    };
    std::vector<WardenCREntry> wardenCREntries_;
    // Module-specific check type opcodes [9]: MEM, PAGE_A, PAGE_B, MPQ, LUA, DRIVER, TIMING, PROC, MODULE
    uint8_t wardenCheckOpcodes_[9] = {};

    // Cheat-check responses being built off the main thread, oldest first.
    //
    // Every check request is answered from a worker: hashing the client files
    // an MPQ check names costs ten milliseconds a file, six to a request, and
    // the code-pattern searches of PAGE_A/PAGE_B cost seconds. update() sends
    // them in the order the requests came, as each front one finishes. Holds
    // plaintext; encryption stays on the main thread, in send order, because
    // the cipher is a stream.
    std::deque<std::future<std::vector<uint8_t>>> wardenPendingResponses_;
    // Held by a worker for the whole of one response: the client image it
    // reads is also written (the tick count a MEM check asks for).
    std::mutex wardenCheckMutex_;
    [[nodiscard]] bool wardenResponsePending() const { return !wardenPendingResponses_.empty(); }
    void cancelPendingWardenResponses();
};

} // namespace game
} // namespace wowee
