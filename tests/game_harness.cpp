#include "game_harness.hpp"

#include <catch_amalgamated.hpp>  // test_support.hpp's REQUIRE
#include "test_support.hpp"

#include "core/env.hpp"
#include "game/entity.hpp"
#include "game/protocol_tables.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packet_data.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
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
    if (refused_) return net::AsyncConnect::Status::Failed;
    connected_ = true;
    return net::AsyncConnect::Status::Connected;
}

void FakeWorldConnection::send(const network::Packet& packet) {
    sent_.push_back(packet);
    if (!answer_ || !connected_) return;
    if (auto reply = answer_(packet)) inbound_.push_back(std::move(*reply));
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

GameHarness::GameHarness() : GameHarness(scratchDir("wowee_harness_account_data")) {}

GameHarness::GameHarness(const std::filesystem::path& accountDataDir) {
    useEmptyExtensionsDir();
    services_.accountDataDir = accountDataDir.string();

    if (expansions_.initialize(sourceRoot() + "Data") == 0 || !expansions_.setActive("wotlk")) {
        fail("no wotlk profile under " + sourceRoot() + "Data/expansions");
    }

    emotes_ = std::make_unique<game::EmoteTable>(nullptr);
    services_.expansionRegistry = &expansions_;
    services_.emotes = emotes_.get();
    services_.events = &events_;
    services_.cvars = &cvars_;
    services_.makeConnection = [this] {
        // What a live server says without being asked twice: CMSG_PING is
        // answered with SMSG_PONG and the same sequence.
        auto answer = [this](const network::Packet& sent) -> std::optional<network::Packet> {
            if (!pingsAnswered_ || !game_) return std::nullopt;
            if (game_->getOpcodeTable().fromWire(sent.getOpcode()) != game::Opcode::CMSG_PING)
                return std::nullopt;
            network::Packet ping = sent;
            ping.setReadPos(0);
            const uint32_t sequence = ping.readUInt32();
            const uint16_t pong = game_->getOpcodeTable().toWire(game::Opcode::SMSG_PONG);
            return network::Packet(pong, Body().u32(sequence).bytes());
        };
        // A connection the game lets go of is no server to play any more.
        auto made = std::make_shared<FakeWorldConnection*>(nullptr);
        auto connection = std::make_unique<FakeWorldConnection>(answer, connectionsRefused_, [this, made] {
            if (connection_ == *made) connection_ = nullptr;
        });
        *made = connection.get();
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
    return charEnumBody(std::vector<PlayerSpec>{p});
}

std::vector<uint8_t> GameHarness::charEnumBody(const std::vector<PlayerSpec>& characters) {
    Body b;
    b.u8(static_cast<uint8_t>(characters.size()));
    for (const PlayerSpec& p : characters) {
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
    }
    return b.bytes();
}

std::vector<uint8_t> GameHarness::loginVerifyWorldBody(const PlayerSpec& p) {
    return Body().u32(p.mapId).f32(p.x).f32(p.y).f32(p.z).f32(0.0f);
}

namespace {

/// One block's fields as the wire carries them: the mask words, then each set
/// field's value in ascending order.
void writeFields(Body& b, const GameHarness::Fields& fields) {
    const int words = fields.empty() ? 0 : fields.rbegin()->first / 32 + 1;
    b.u8(static_cast<uint8_t>(words));
    for (int w = 0; w < words; ++w) {
        uint32_t mask = 0;
        for (const auto& [field, value] : fields) {
            if (field / 32 == w) mask |= 1u << (field % 32);
        }
        b.u32(mask);
    }
    for (const auto& [field, value] : fields) b.u32(value);
}

} // namespace

void GameHarness::putGuid(Fields& fields, uint16_t field, uint64_t guid) {
    fields[field] = static_cast<uint32_t>(guid);
    fields[static_cast<uint16_t>(field + 1)] = static_cast<uint32_t>(guid >> 32);
}

std::vector<uint8_t> GameHarness::createItemBody(uint64_t guid, uint32_t entry, uint32_t stack) {
    Fields fields;
    fields[game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY)] = entry;
    fields[game::fieldIndex(game::UF::ITEM_FIELD_STACK_COUNT)] = stack;
    Body b;
    b.u32(1);                       // one block
    b.u8(2);                        // CREATE_OBJECT
    b.packedGuid(guid);
    b.u8(1);                        // ITEM
    b.u8(0).u8(0);                  // update flags: no movement
    writeFields(b, fields);
    return b.bytes();
}

std::vector<uint8_t> GameHarness::valuesBody(uint64_t guid, const Fields& fields) {
    Body b;
    b.u32(1);
    b.u8(0);                        // VALUES
    b.packedGuid(guid);
    writeFields(b, fields);
    return b.bytes();
}

void GameHarness::playerInView() {
    const uint64_t guid = player_.guid;
    game_->getEntityManager().addEntity(guid, std::make_shared<game::Player>(guid));
}

void GameHarness::enterWorld(const PlayerSpec& player) {
    player_ = player;
    const std::vector<uint8_t> sessionKey(40, 0x5A);
    if (!game_->connect("harness.test", 8085, sessionKey, "HARNESS", 12340, 1)) {
        fail("connect refused");
    }
    answerLogin(player);
    game_->selectCharacter(player.guid);
    answerPlayerLogin(player);
    clearRecords();
}

void GameHarness::answerLogin(const PlayerSpec& player) {
    using game::Opcode;
    using game::WorldState;
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
}

void GameHarness::answerPlayerLogin(const PlayerSpec& player) {
    using game::Opcode;
    player_ = player;
    if (!lastSent(Opcode::CMSG_PLAYER_LOGIN)) fail("no CMSG_PLAYER_LOGIN");
    receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, loginVerifyWorldBody(player));
    pump();
    if (game_->getState() != game::WorldState::IN_WORLD)
        fail("not in the world after LOGIN_VERIFY_WORLD");
}

std::vector<uint8_t> GameHarness::createObjectBody(uint64_t guid, game::ObjectType type,
                                                   std::vector<std::pair<uint16_t, uint32_t>> fields,
                                                   float x, float y, float z) {
    for (const auto& [index, value] : fields) {
        (void)value;
        if (index == 0xFFFF) fail("an update field the active table does not have");
    }
    std::sort(fields.begin(), fields.end());
    const int words = fields.empty() ? 1 : fields.back().first / 32 + 1;
    std::vector<uint32_t> mask(static_cast<size_t>(words), 0);
    for (const auto& [index, value] : fields) mask[index / 32] |= 1u << (index % 32);

    const bool living = type == game::ObjectType::UNIT || type == game::ObjectType::PLAYER;
    Body b;
    b.u32(1);                                            // one block
    b.u8(static_cast<uint8_t>(game::UpdateType::CREATE_OBJECT));
    b.packedGuid(guid);
    b.u8(static_cast<uint8_t>(type));
    if (living) {
        b.u8(0x20).u8(0x00);                             // update flags: living
        b.u32(0).u8(0).u8(0);                            // movement flags, flags2
        b.u32(0);                                        // time
        b.f32(x).f32(y).f32(z).f32(0.0f);                // position, facing
        b.u32(0);                                        // fall time
        for (float speed : {2.5f, 7.0f, 4.5f, 4.72f, 2.5f, 7.0f, 4.5f, 3.14159f, 3.14159f})
            b.f32(speed);                                // walk .. pitch rate
    } else {
        b.u8(0x00).u8(0x00);                             // update flags: none
    }
    b.u8(static_cast<uint8_t>(words));
    for (uint32_t word : mask) b.u32(word);
    for (const auto& [index, value] : fields) {
        (void)index;
        b.u32(value);
    }
    return b.bytes();
}

void GameHarness::relogAs(const PlayerSpec& next) {
    using game::Opcode;
    const PlayerSpec previous = player_;
    player_ = next;

    game_->requestLogout(false);
    receive(Opcode::SMSG_LOGOUT_COMPLETE);
    pump();
    game_->returnToCharacterSelect();
    receive(Opcode::SMSG_CHAR_ENUM, charEnumBody({previous, next}));
    pump();
    game_->selectCharacter(next.guid);
    receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, loginVerifyWorldBody(next));
    pump();
    if (!game_->isInWorld()) fail("the next character is not in the world");

    clearRecords();
}

} // namespace wowee::test
