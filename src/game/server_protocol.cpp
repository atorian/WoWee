#include "game/server_protocol.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <openssl/evp.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace wowee::game {
namespace {
uint32_t le32(std::span<const uint8_t> bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
           uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
class EzWowProtocol final : public ServerProtocolAdapter {
    std::string error_;
    ProtocolEvents events_;
    std::array<uint8_t,216> bootstrap_{};
    std::array<bool,216> received_{};
    bool receiving_ = false;
    bool installed_ = false;
    bool translated_ = false;
    bool patchesRequested_ = false;
    bool manifestReceived_ = false;
    bool earlyEnumerationPending_ = false;
    bool enumerationRetryRequested_ = false;
    bool enumerationRetryPending_ = false;
    std::vector<EzWowPatch> patchManifest_;
    void validateBootstrap() {
        if (!std::all_of(received_.begin(), received_.end(), [](bool b) { return b; })) return;
        unsigned char hash[EVP_MAX_MD_SIZE]{}; unsigned size = 0;
        if (!EVP_Digest(bootstrap_.data(), bootstrap_.size(), hash, &size, EVP_sha256(), nullptr))
            throw std::runtime_error("cannot hash the delivered bootstrap");
        constexpr uint8_t expected[] = {0x6a,0x4b,0x09,0xa4,0x4a,0xed,0x29,0xe1,0xae,0x0e,0x42,0x23,0x75,0x74,0x01,0x8e,0xe7,0x9c,0x0a,0x19,0x23,0xfa,0xfd,0xfc,0xdc,0x76,0x8b,0x5d,0x84,0x1d,0x28,0x6e};
        // Recognize the callback contract, never synthesize its integrity bytes.
        translated_ = size == sizeof(expected) && std::equal(std::begin(expected), std::end(expected), hash);
        if (!translated_) throw std::runtime_error("the delivered bootstrap version has no verified translation");
    }
public:
    explicit EzWowProtocol(ProtocolEvents events) : events_(std::move(events)) {}
    void warden(std::span<const uint8_t> message) override {
        if (message.empty() || message[0] != 3) return;
        try {
            // Validate every record before applying any callback changes.
            std::vector<std::span<const uint8_t>> bodies;
            size_t position = 0;
            while (position < message.size()) {
                if (message.size()-position < 7 || message[position] != 3)
                    throw std::runtime_error("truncated module initialization header");
                const size_t length = size_t(message[position+1]) | size_t(message[position+2]) << 8;
                if (!length || length > 4096 || length > message.size()-position-7)
                    throw std::runtime_error("invalid module initialization size");
                auto body = message.subspan(position+7, length);
                unsigned char hash[EVP_MAX_MD_SIZE]{}; unsigned size = 0;
                if (!EVP_Digest(body.data(), body.size(), hash, &size, EVP_sha1(), nullptr) || size != 20)
                    throw std::runtime_error("cannot hash initialization record");
                uint32_t checksum = 0;
                for (int i=0; i<5; ++i) checksum ^= le32(std::span<const uint8_t>(hash,20).subspan(i*4,4));
                if (checksum != le32(message.subspan(position+3,4)))
                    throw std::runtime_error("module initialization checksum mismatch");
                bodies.push_back(body); position += 7+length;
            }
            // In four reference exchanges, the final initialization replaces
            // the file callback with FrameScriptExecute (RVA 0x419210). Earlier
            // restores retain the MPQ callback (0x24F80). Require this boundary
            // and the manifest, rather than guessing readiness from elapsed time.
            const bool finalFileCallback = std::any_of(bodies.begin(), bodies.end(), [](auto body) {
                return body.size() == 20 && body[0] == 1 && body[1] == 0 &&
                       body[2] == 2 && body[3] == 0 && le32(body.subspan(4,4)) == 0x419210;
            });
            for (auto body : bodies) {
                if (body.size() != 8 || body[0] != 1 || body[1] != 1 || body[2] != 0 || body[7] != 1) continue;
                const auto callback = le32(body.subspan(3,4));
                if (callback == 0x14E720) {
                    bootstrap_.fill(0); received_.fill(false); patchManifest_.clear();
                    receiving_ = true; installed_ = translated_ = patchesRequested_ = false;
                    manifestReceived_ = enumerationRetryRequested_ = enumerationRetryPending_ = false;
                    earlyEnumerationPending_ = false;
                } else if (callback == 0x46AE20) {
                    if (receiving_) {
                        receiving_ = false; installed_ = translated_;
                        // The delivered callback is installed. Try the ordinary
                        // character query now; manifest/final retries remain if
                        // the server still gates enumeration at this stage.
                        if (installed_) earlyEnumerationPending_ = true;
                    } else if (installed_ && patchesRequested_ && manifestReceived_ &&
                               finalFileCallback && !enumerationRetryRequested_) {
                        // Queue only a retry of the ordinary query; no extension
                        // readiness or patch-installation acknowledgement is sent.
                        enumerationRetryRequested_ = enumerationRetryPending_ = true;
                    }
                }
            }
        } catch (const std::exception& e) { error_ = std::string("EZWoW: ") + e.what(); }
    }
    ProtocolResult packet(uint16_t opcode, std::span<const uint8_t> body,
                          const ProtocolSend& send) override {
        if (opcode == 0x2E9 && receiving_) {
            // The original MSG_BATTLEGROUND_PLAYER_POSITIONS handler decrements
            // the second count before writing each GUID to 0xBEA170+index*8.
            // A short packet writes just the available GUID: failed CDataStore
            // reads leave the remaining destination bytes untouched. Do not
            // reproduce its huge count loop or write into native process memory.
            if (body.size() < 8 || le32(body.first(4)) != 0) return ProtocolResult::Unhandled;
            const auto count = le32(body.subspan(4,4));
            if (!count) return ProtocolResult::Unhandled;
            const uint64_t address = 0xBEA170ull + (uint64_t(count)-1)*8;
            if (address < 0xDD1000 || address >= 0xDD10D8) return ProtocolResult::Unhandled;
            if (body.size() != 16 || address+8 > 0xDD10D8) {
                error_ = "EZWoW: malformed bootstrap write packet"; return ProtocolResult::Error;
            }
            const size_t offset = size_t(address-0xDD1000);
            std::copy_n(body.begin()+8,8,bootstrap_.begin()+offset);
            std::fill_n(received_.begin()+offset,8,true);
            try { validateBootstrap(); }
            catch (const std::exception& e) { error_ = std::string("EZWoW: ") + e.what(); return ProtocolResult::Error; }
            return ProtocolResult::Handled;
        }
        if (opcode != 0x41D) return ProtocolResult::Unhandled;
        try {
            if (body.size() < 4) throw std::runtime_error("missing extension command");
            const auto command = le32(body.first(4));
            if (command == 1) {
                if (!installed_ || !patchesRequested_)
                    throw std::runtime_error("patch metadata arrived before the loader request");
                // Receiving the manifest does not claim that archives are loaded.
                // This callback sends no immediate reply in the reference DLL.
                auto patches = parseEzWowPatches(body);
                patchManifest_ = std::move(patches);
                if (events_.patchManifest) events_.patchManifest(patchManifest_);
                // Native processFinishDone switches to charselect, whose OnShow
                // calls GetCharacterListUpdate. Allow that ordinary query to
                // overlap the remaining checks, retaining the final retry.
                if (!manifestReceived_) earlyEnumerationPending_ = true;
                manifestReceived_ = true;
                return ProtocolResult::Handled;
            }
            if (command == 3) {
                if (!installed_ || !patchesRequested_)
                    throw std::runtime_error("known titles arrived before the loader request");
                const auto update = parseEzWowKnownTitles(body);
                if (!events_.knownTitles) throw std::runtime_error("known-title callback is unavailable");
                events_.knownTitles(update);
                return ProtocolResult::Handled; // Native onOpcodeKnownTitles sends no reply.
            }
            if (command == 5) {
                if (!installed_ || !patchesRequested_)
                    throw std::runtime_error("money update arrived before the loader request");
                const auto copper = parseEzWowMoney(body);
                if (!events_.money) throw std::runtime_error("money callback is unavailable");
                events_.money(copper);
                return ProtocolResult::Handled;
            }
            if (command != 0) {
                error_ = "EZWoW: extension command " + std::to_string(command) +
                    " (" + std::to_string(body.size()-4) + " bytes) needs translation; see the protocol trace.";
                return ProtocolResult::Error;
            }
            const auto payload = parseEzWowPayload(body);
            if (!installed_) throw std::runtime_error("loader arrived before bootstrap installation");
            constexpr uint8_t loaderHash[] = {0xb9,0xa8,0x00,0xae,0xba,0x89,0x81,0x10,0x23,0xcf,0x9b,0x34,0x2a,0x05,0xed,0xb8,0xee,0x2b,0xfe,0xa7,0xed,0x47,0xa3,0x74,0xec,0xc2,0xe6,0xc8,0x30,0x9f,0x5c,0x2b};
            unsigned char hash[EVP_MAX_MD_SIZE]{}; unsigned size = 0;
            if (!EVP_Digest(payload.code.data(), payload.code.size(), hash, &size, EVP_sha256(), nullptr)
                || size != sizeof(loaderHash) || !std::equal(std::begin(loaderHash),std::end(loaderHash),hash))
                throw std::runtime_error("executable loader version has no verified translation");
            // Verified loader arguments: sized filename, file size, flags, MD5.
            // This release's LE command installs the custom dispatch and requests
            // patches. Implement that transport step without reporting DLL load
            // success or executing the downloaded x86/Windows code.
            constexpr uint8_t manifest[] = {9,0,0,0,'e','z','L','i','b','.','d','l','l',
                0xc0,0x8b,0x3e,0,0,0,0,0,0xdc,0xce,0x81,0x90,0xb7,0xe7,0x7a,0x58,
                0x22,0x67,0x22,0x22,0xc7,0xe8,0xfe,0x76};
            if (!payload.trailing.empty() || payload.arguments.size() != sizeof(manifest)
                || !std::equal(std::begin(manifest),std::end(manifest),payload.arguments.begin()))
                throw std::runtime_error("unsupported ezLib release or loader arguments");
            if (!patchesRequested_) {
                send(0x51F, {}); // Patches::onLoadGeneral, original ezLib+0xA086C.
                patchesRequested_ = true;
            }
            return ProtocolResult::Handled;
        } catch (const std::exception& e) {
            error_ = std::string("EZWoW: invalid extension packet: ") + e.what();
        }
        return ProtocolResult::Error;
    }
    ProtocolResult memory(const std::string& module, uint32_t address,
                          uint8_t length, uint8_t* output) override {
        auto normalized = module;
        for (auto& c : normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((!normalized.empty() && normalized != "wow.exe") || !length ||
            uint64_t(address) + length <= 0xDD1000 || address >= 0xDD10D8)
            return ProtocolResult::Unhandled;
        if (installed_ && address >= 0xDD1000 && uint64_t(address)+length <= 0xDD10D8) {
            std::copy_n(bootstrap_.begin()+(address-0xDD1000),length,output);
            return ProtocolResult::Handled;
        }
        error_ = "EZWoW: runtime bootstrap at 0xDD1000 was not fully received and installed for this session.";
        return ProtocolResult::Error;
    }
    bool takeEarlyCharacterListRetryRequest() override {
        const bool pending = earlyEnumerationPending_ && error_.empty();
        earlyEnumerationPending_ = false;
        return pending;
    }
    bool takeCharacterListRetryRequest() override {
        const bool pending = enumerationRetryPending_ && error_.empty();
        enumerationRetryPending_ = false;
        return pending;
    }
    std::string error() const override { return error_; }
};
}
bool isServerProtocolDiagnosticOpcode(uint16_t opcode) {
    // 0x54E720 is selected as EZWoW's initial timing callback. The original
    // function registers exactly these battleground packet handlers; observe
    // them without changing their normal dispatch until the carrier is known.
    constexpr uint16_t channels[] = {0x41D, 0x23D, 0x2D4, 0x2E0, 0x2E8,
        0x2E9, 0x2EC, 0x2ED, 0x4C7, 0x14B, 0x4DE, 0x4E0, 0x4E4, 0x4E5,
        0x4E6, 0x4E1, 0x4E8};
    return std::find(std::begin(channels), std::end(channels), opcode) != std::end(channels);
}
std::unique_ptr<ServerProtocolAdapter> makeServerProtocol(
    const std::string& name, const ProtocolSession& session) {
    if (name != "ezwow") throw std::runtime_error("Unknown built-in server protocol: " + name);
    if (session.build != 12340) throw std::runtime_error("EZWoW protocol requires client build 12340");
    return std::make_unique<EzWowProtocol>(session.events);
}
uint64_t parseEzWowMoney(std::span<const uint8_t> body) {
    if (body.size() != 12 || le32(body.first(4)) != 5)
        throw std::runtime_error("invalid money update length or command");
    const uint64_t bits = le32(body.subspan(4,4)) | uint64_t(le32(body.subspan(8,4))) << 32;
    const double copper = std::bit_cast<double>(bits);
    // Currency is integral. Keep conversions within IEEE754's exact integer
    // range and reject NaN/infinity before casting to a native integer.
    if (!std::isfinite(copper) || copper < 0 || copper > 9007199254740991.0 || std::floor(copper) != copper)
        throw std::runtime_error("money update is outside the supported exact integer range");
    return static_cast<uint64_t>(copper);
}
KnownTitleUpdate parseEzWowKnownTitles(std::span<const uint8_t> body) {
    if (body.size() < 7 || le32(body.first(4)) != 3)
        throw std::runtime_error("invalid known-title header");
    const uint8_t operation = body[4];
    if (operation < 1 || operation > 3) throw std::runtime_error("unknown known-title operation");
    const size_t count = size_t(body[5]) | size_t(body[6]) << 8;
    if (body.size()-7 != count*2) throw std::runtime_error("invalid known-title count or trailing bytes");
    KnownTitleUpdate update{operation,{}};
    update.bits.reserve(count);
    for (size_t i=7; i<body.size(); i+=2)
        update.bits.push_back(uint16_t(body[i]) | uint16_t(body[i+1]) << 8);
    return update;
}
EzWowPayload parseEzWowPayload(std::span<const uint8_t> body) {
    constexpr size_t maxBody = 1024 * 1024;
    if (body.size() < 12 || body.size() > maxBody)
        throw std::runtime_error("payload size is outside the supported bounds");
    const auto command = le32(body.first(4));
    if (command != 0) throw std::runtime_error("command is not an executable loader");
    const auto offset = le32(body.subspan(4,4));
    // Subtract before adding: even UINT32_MAX cannot wrap into a valid offset.
    if (offset > body.size() - 12) throw std::runtime_error("code offset exceeds the packet");
    const size_t position = 8 + size_t(offset);
    const auto length = le32(body.subspan(position, 4));
    if (!length || length > body.size() - position - 4)
        throw std::runtime_error("code length exceeds the packet or is zero");
    return {command, body.subspan(8, offset), body.subspan(position + 4, length),
            body.subspan(position + 4 + length)};
}
std::vector<EzWowPatch> parseEzWowPatches(std::span<const uint8_t> body) {
    if (body.size() < 5 || body.size() > 1024*1024 || le32(body.first(4)) != 1)
        throw std::runtime_error("invalid patch manifest header");
    const size_t count = body[4];
    std::vector<EzWowPatch> patches;
    patches.reserve(count);
    size_t position = 5;
    for (size_t i=0; i<count; ++i) {
        if (body.size()-position < 4) throw std::runtime_error("truncated patch name length");
        const size_t length = le32(body.subspan(position,4)); position += 4;
        if (!length || length > 255 || length > body.size()-position
            || body.size()-position-length < 40)
            throw std::runtime_error("invalid patch record length");
        const auto nameBytes = body.subspan(position,length); position += length;
        // Server names are archive basenames. Reject directory traversal before
        // any future storage/mounting code can use the manifest.
        if (std::any_of(nameBytes.begin(),nameBytes.end(),[](uint8_t c) {
                return c < 0x20 || c >= 0x7F || c == '/' || c == '\\' || c == ':';
            })) throw std::runtime_error("invalid patch archive name");
        std::string name(nameBytes.begin(),nameBytes.end());
        auto lower = name;
        for (auto& c : lower) if(c>='A' && c<='Z') c += 'a'-'A';
        if (lower.find("..") != std::string::npos || !lower.ends_with(".mpq"))
            throw std::runtime_error("patch name is not an archive basename");
        for (const auto& existing : patches) {
            auto previous = existing.name;
            for (auto& c : previous) if(c>='A' && c<='Z') c += 'a'-'A';
            if (previous == lower) throw std::runtime_error("duplicate patch archive name");
        }
        EzWowPatch patch{name,le32(body.subspan(position,4)),le32(body.subspan(position+4,4)),{}, {}};
        if (!patch.size) throw std::runtime_error("zero patch archive size");
        std::copy_n(body.begin()+position+8,16,patch.checksum.begin());
        std::copy_n(body.begin()+position+24,16,patch.fastChecksum.begin());
        position += 40; patches.push_back(std::move(patch));
    }
    if (position != body.size()) throw std::runtime_error("trailing patch manifest bytes");
    return patches;
}
struct ServerProtocolTrace::Impl {
    std::filesystem::path path;
    std::ofstream output;
    uint64_t bytes = 4;
    static constexpr uint64_t cap = 8 * 1024 * 1024;
};
ServerProtocolTrace::ServerProtocolTrace(const std::filesystem::path& directory)
    : impl_(std::make_unique<Impl>()) {
    static std::atomic<uint64_t> sequence{0};
    std::filesystem::create_directories(directory);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    impl_->path = directory / ("session-" + std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count()) +
        "-" + std::to_string(sequence++) + ".wpt");
    impl_->output.open(impl_->path, std::ios::binary | std::ios::trunc);
    impl_->output.write("WPT1", 4);
    impl_->output.flush();
    if (!impl_->output) throw std::runtime_error("Cannot create server protocol trace");
}
ServerProtocolTrace::~ServerProtocolTrace() = default;
const std::filesystem::path& ServerProtocolTrace::path() const { return impl_->path; }
void ServerProtocolTrace::record(uint32_t channel, uint16_t opcode, std::span<const uint8_t> body) {
    // Explicit allowlist, including if a caller accidentally forwards auth traffic.
    if (!((channel == 1 && !body.empty() && (body[0] == 2 || body[0] == 3) && opcode == 0x2E6)
        || (channel == 2 && isServerProtocolDiagnosticOpcode(opcode)))) return;
    if (body.size() > 1024 * 1024 || body.size() + 12 > Impl::cap - impl_->bytes) return;
    const uint32_t values[] = {channel, opcode, static_cast<uint32_t>(body.size())};
    for (auto value : values) for (int i = 0; i < 4; ++i)
        impl_->output.put(static_cast<char>(value >> (8 * i)));
    impl_->output.write(reinterpret_cast<const char*>(body.data()), body.size());
    impl_->output.flush();
    if (!impl_->output) throw std::runtime_error("Cannot write server protocol trace");
    impl_->bytes += 12 + body.size();
}
}
