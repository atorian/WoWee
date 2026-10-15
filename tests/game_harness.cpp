#include "game_harness.hpp"

#include <catch_amalgamated.hpp>  // test_support.hpp's REQUIRE
#include "test_support.hpp"

#include "core/env.hpp"
#include "game/protocol_tables.hpp"
#include "game/world_packet_data.hpp"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace wowee::test {

// ---- FakeWorldConnection ----------------------------------------------------

void FakeWorldConnection::beginConnect(const std::string&, uint16_t, int) {
    connecting_ = true;
}

net::AsyncConnect::Status FakeWorldConnection::pollConnect() {
    if (connected_) return net::AsyncConnect::Status::Connected;
    if (!connecting_) return net::AsyncConnect::Status::Idle;
    connecting_ = false;
    connected_ = true;
    return net::AsyncConnect::Status::Connected;
}

void FakeWorldConnection::update() {
    while (!inbound_.empty() && callback_) {
        network::Packet packet = std::move(inbound_.front());
        inbound_.pop_front();
        callback_(packet);
    }
}

// ---- Body -------------------------------------------------------------------

Body& Body::u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
}

Body& Body::u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
}

Body& Body::f32(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return u32(bits);
}

Body& Body::cstr(const std::string& s) {
    bytes_.insert(bytes_.end(), s.begin(), s.end());
    bytes_.push_back(0);
    return *this;
}

Body& Body::packedGuid(uint64_t guid) {
    uint8_t mask = 0;
    std::vector<uint8_t> parts;
    for (int i = 0; i < 8; ++i) {
        const uint8_t byte = static_cast<uint8_t>(guid >> (8 * i));
        if (byte != 0) {
            mask |= static_cast<uint8_t>(1u << i);
            parts.push_back(byte);
        }
    }
    bytes_.push_back(mask);
    bytes_.insert(bytes_.end(), parts.begin(), parts.end());
    return *this;
}

// ---- GameHarness ------------------------------------------------------------

namespace {

/// No server extensions in a test: the game looks for one beside the client
/// for every host it connects to, and a machine with Data/server_extensions
/// filled in must not change what a test sees.
void useEmptyExtensionsDir() {
    const std::string dir = scratchDir("wowee_harness_extensions").string();
    core::setEnvVar("WOWEE_SERVER_EXTENSIONS_DIR", dir.c_str());
}

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("GameHarness: " + what);
}

} // namespace

GameHarness::GameHarness() {
    useEmptyExtensionsDir();

    if (expansions_.initialize(sourceRoot() + "Data") == 0 || !expansions_.setActive("wotlk")) {
        fail("no wotlk profile under " + sourceRoot() + "Data/expansions");
    }

    emotes_ = std::make_unique<game::EmoteTable>(nullptr);
    services_.expansionRegistry = &expansions_;
    services_.emotes = emotes_.get();
    services_.events = &events_;
    services_.cvars = &cvars_;
    services_.makeConnection = [this] {
        auto connection = std::make_unique<FakeWorldConnection>();
        connection_ = connection.get();
        return connection;
    };
    cvars_.load();

    game_ = std::make_unique<game::GameHandler>(services_);
    game::loadProtocolTables(*game_, *expansions_.getActive());
    game_->setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
        addonEvents_.push_back({name, args});
    });
}

GameHarness::~GameHarness() = default;

FakeWorldConnection& GameHarness::server() {
    if (!connection_) fail("no connection yet: connect (or enterWorld) first");
    return *connection_;
}

void GameHarness::receive(game::LogicalOpcode op, const std::vector<uint8_t>& body) {
    const uint16_t wire = game_->getOpcodeTable().toWire(op);
    if (wire == 0xFFFF) fail("opcode has no wire value in the active table");
    receive(network::Packet(wire, body));
}

void GameHarness::receive(network::Packet packet) {
    server().deliver(std::move(packet));
}

void GameHarness::pump() {
    // A frame delivers what arrived and handles it within a time budget, so a
    // burst can take a few. A thousand is far past any test's burst and short
    // of hanging the run when something never drains.
    for (int frame = 0; frame < 1000; ++frame) {
        game_->update(1.0f / 60.0f);
        game_->dispatchAddonEvents();
        if (game_->queuedIncomingPacketCount() == 0) return;
    }
    fail("packets still queued after 1000 frames");
}

void GameHarness::advance(float seconds) {
    constexpr float kFrame = 1.0f / 60.0f;
    for (float passed = 0.0f; passed < seconds; passed += kFrame) {
        game_->update(kFrame);
        game_->dispatchAddonEvents();
    }
}

std::vector<game::LogicalOpcode> GameHarness::sentOpcodes() const {
    std::vector<game::LogicalOpcode> out;
    if (!connection_) return out;
    for (const auto& packet : connection_->sent()) {
        if (auto op = game_->getOpcodeTable().fromWire(packet.getOpcode())) out.push_back(*op);
    }
    return out;
}

network::Packet* GameHarness::lastSent(game::LogicalOpcode op) {
    if (!connection_) return nullptr;
    auto& sent = connection_->sent();
    for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
        if (game_->getOpcodeTable().fromWire(it->getOpcode()) == op) {
            it->setReadPos(0);
            return &*it;
        }
    }
    return nullptr;
}

size_t GameHarness::countSent(game::LogicalOpcode op) const {
    size_t n = 0;
    for (auto sent : sentOpcodes()) n += (sent == op);
    return n;
}

std::vector<AddonEvent> GameHarness::addonEvents(const std::string& name) const {
    std::vector<AddonEvent> out;
    for (const auto& e : addonEvents_) {
        if (e.name == name) out.push_back(e);
    }
    return out;
}

void GameHarness::clearRecords() {
    addonEvents_.clear();
    if (connection_) connection_->clearSent();
}

std::vector<uint8_t> GameHarness::authChallengeBody(uint32_t serverSeed) {
    // WotLK: a one, the seed, and 32 bytes of the server's own seeds.
    return Body().u32(1).u32(serverSeed).zeros(32);
}

std::vector<uint8_t> GameHarness::charEnumBody(const PlayerSpec& p) {
    Body b;
    b.u8(1);                       // one character
    b.u64(p.guid).cstr(p.name);
    b.u8(p.race).u8(p.classId).u8(0 /* male */);
    b.u32(0).u8(0);                // skin, face, hair style and colour; facial hair
    b.u8(p.level);
    b.u32(12 /* Elwynn */).u32(p.mapId).f32(p.x).f32(p.y).f32(p.z);
    b.u32(0);                      // guild
    b.u32(0);                      // character flags
    b.u32(0).u8(0);                // customization flags, first login
    b.u32(0).u32(0).u32(0);        // pet: display, level, family
    for (int slot = 0; slot < 23; ++slot) {
        b.u32(0).u8(0).u32(0);     // display, inventory type, enchant aura
    }
    return b.bytes();
}

std::vector<uint8_t> GameHarness::loginVerifyWorldBody(const PlayerSpec& p) {
    return Body().u32(p.mapId).f32(p.x).f32(p.y).f32(p.z).f32(0.0f);
}

void GameHarness::enterWorld(const PlayerSpec& player) {
    player_ = player;
    using game::Opcode;
    using game::WorldState;

    const std::vector<uint8_t> sessionKey(40, 0x5A);
    if (!game_->connect("harness.test", 8085, sessionKey, "HARNESS", 12340, 1)) {
        fail("connect refused");
    }
    pump();
    if (game_->getState() != WorldState::CONNECTED) fail("did not connect");

    receive(Opcode::SMSG_AUTH_CHALLENGE, authChallengeBody(0xC0FFEE));
    pump();
    if (!lastSent(Opcode::CMSG_AUTH_SESSION)) fail("no CMSG_AUTH_SESSION");

    receive(Opcode::SMSG_AUTH_RESPONSE, Body().u8(0x0C));  // AUTH_OK
    pump();
    if (!lastSent(Opcode::CMSG_CHAR_ENUM)) fail("no CMSG_CHAR_ENUM after AUTH_OK");

    receive(Opcode::SMSG_CHAR_ENUM, charEnumBody(player));
    pump();
    if (game_->getState() != WorldState::CHAR_LIST_RECEIVED) fail("character list not taken");

    game_->selectCharacter(player.guid);
    if (!lastSent(Opcode::CMSG_PLAYER_LOGIN)) fail("no CMSG_PLAYER_LOGIN");

    receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, loginVerifyWorldBody(player));
    pump();
    if (game_->getState() != WorldState::IN_WORLD) fail("not in the world after LOGIN_VERIFY_WORLD");

    clearRecords();
}

} // namespace wowee::test
