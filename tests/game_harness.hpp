#pragma once

/// A GameHandler talking WotLK to a server that is the test.
///
/// The real game - its session, its domain handlers, the state they build -
/// linked from wowee_game, with nothing of the client: no renderer, no sound,
/// no interface. The test plays the server's side with packets and reads what
/// the game did with them from the three places the rest of the client would:
///
///   * what the game sent back, from the connection it was handed;
///   * what it told the interface, the FrameXML events, in order;
///   * what it announced on the event bus, by subscribing (GameHarness::events).
///
/// So a test can follow a path all the way through - a packet in, the state
/// it builds, what the interface is told, and the command that reads that
/// state back out - which is where a copy of the state that one side writes
/// and the other reads shows up.

#include "core/cvar_store.hpp"
#include "core/event_bus.hpp"
#include "game/emote_table.hpp"
#include "game/entity.hpp"
#include "game/expansion_profile.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include "network/world_connection.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wowee::test {

/// The world connection as the server end of a test sees it. Connects at once,
/// keeps what the game sent, and delivers what the test queues on the next
/// update - the same frame order a socket gives.
class FakeWorldConnection final : public network::WorldConnection {
public:
    /// What the server answers by itself to a packet the game sent, if
    /// anything: delivered on the next update like any other.
    using Answer = std::function<std::optional<network::Packet>(const network::Packet&)>;
    /// `refused`: the server is not there; the connect fails.
    FakeWorldConnection(Answer answer, bool refused, std::function<void()> destroyed)
        : answer_(std::move(answer)), refused_(refused), destroyed_(std::move(destroyed)) {}
    ~FakeWorldConnection() override {
        if (destroyed_) destroyed_();
    }

    void beginConnect(const std::string& host, uint16_t port, int timeoutSeconds) override;
    net::AsyncConnect::Status pollConnect() override;
    [[nodiscard]] std::string connectError() const override {
        return refused_ ? "connection refused (the test's server is down)" : std::string();
    }
    void disconnect() override { connected_ = false; }
    [[nodiscard]] bool isConnected() const override { return connected_; }
    void send(const network::Packet& packet) override;
    void update() override;
    void setPacketCallback(std::function<void(const network::Packet&)> callback) override {
        callback_ = std::move(callback);
    }
    void initEncryption(const std::vector<uint8_t>&, uint32_t) override { encrypted_ = true; }
    void tracePacketsFor(std::chrono::milliseconds, const std::string&) override {}

    /// A packet from the server, delivered on the game's next update.
    void deliver(network::Packet packet) { inbound_.push_back(std::move(packet)); }
    [[nodiscard]] const std::vector<network::Packet>& sent() const { return sent_; }
    [[nodiscard]] std::vector<network::Packet>& sent() { return sent_; }
    void clearSent() { sent_.clear(); }
    [[nodiscard]] bool encrypted() const { return encrypted_; }

private:
    Answer answer_;
    bool refused_ = false;
    std::function<void()> destroyed_;
    bool connecting_ = false;
    bool connected_ = false;
    bool encrypted_ = false;
    std::function<void(const network::Packet&)> callback_;
    std::deque<network::Packet> inbound_;
    std::vector<network::Packet> sent_;
};

/// One FrameXML event the game fired, with its arguments.
struct AddonEvent {
    std::string name;
    std::vector<std::string> args;
};

/// The player the harness logs in with.
struct PlayerSpec {
    uint64_t guid = 0x10;
    std::string name = "Harness";
    uint8_t race = 1;    // Human
    uint8_t classId = 1; // Warrior
    uint8_t level = 10;
    uint32_t mapId = 0;  // Eastern Kingdoms
    float x = -8949.95f, y = -132.49f, z = 83.53f;  // Northshire, server coordinates
};

class GameHarness {
public:
    /// With an empty folder of its own for the account data the game keeps on
    /// disk.
    GameHarness();
    /// With the account data kept in this folder, as it is - for a test of
    /// what one session leaves behind for the next.
    explicit GameHarness(const std::filesystem::path& accountDataDir);
    ~GameHarness();
    GameHarness(const GameHarness&) = delete;
    GameHarness& operator=(const GameHarness&) = delete;

    game::GameHandler& game() { return *game_; }
    core::EventBus& events() { return events_; }
    core::CVarStore& cvars() { return cvars_; }

    /// The server's side of the connection: the one the game made last. Valid
    /// once connect() has run, which enterWorld does, until the game lets go
    /// of it.
    FakeWorldConnection& server();
    /// Whether the game holds a connection the test can play the server of.
    [[nodiscard]] bool hasServer() const { return connection_ != nullptr; }

    /// A live server answers CMSG_PING with SMSG_PONG, and this one does
    /// unless told not to: a server gone silent with the connection open.
    void setPingsAnswered(bool answered) { pingsAnswered_ = answered; }
    /// The connections the game makes from now on fail to connect: the
    /// server is down.
    void setConnectionsRefused(bool refused) { connectionsRefused_ = refused; }

    /// A packet from the server: the opcode, as the active table puts it on
    /// the wire, and the body.
    void receive(game::LogicalOpcode op, const std::vector<uint8_t>& body = {});
    void receive(network::Packet packet);

    /// Run the game's frame until everything received has been handled.
    /// Fails the test if it never is.
    void pump();
    /// Let this much game time pass, a frame at a time - for the timers the
    /// game counts down by the frame's delta.
    void advance(float seconds);

    /// What the game sent, as logical opcodes, in order.
    [[nodiscard]] std::vector<game::LogicalOpcode> sentOpcodes() const;
    /// The last packet of this opcode the game sent, or nullptr. Read from the
    /// start of its body.
    [[nodiscard]] network::Packet* lastSent(game::LogicalOpcode op);
    [[nodiscard]] size_t countSent(game::LogicalOpcode op) const;

    /// Every FrameXML event fired, in order, and those with one name.
    [[nodiscard]] const std::vector<AddonEvent>& addonEvents() const { return addonEvents_; }
    [[nodiscard]] std::vector<AddonEvent> addonEvents(const std::string& name) const;

    /// Forget what was sent and fired so far, to look at one step on its own.
    void clearRecords();

    /// Connect, authenticate, list one character and log it into the world.
    /// Ends IN_WORLD with nothing recorded.
    void enterWorld(const PlayerSpec& player = {});
    /// Log out to the character list, on the same connection, and log the
    /// next character of the account in. Ends IN_WORLD with nothing recorded.
    void relogAs(const PlayerSpec& next);
    /// The server's half of a login on the connection the game has just
    /// begun: greet it, take its AUTH_SESSION, list `player`. Ends
    /// CHAR_LIST_RECEIVED.
    void answerLogin(const PlayerSpec& player);
    /// The server takes the character the game asked for into the world.
    /// Ends IN_WORLD.
    void answerPlayerLogin(const PlayerSpec& player);
    [[nodiscard]] const PlayerSpec& player() const { return player_; }

    /// The server's packets the harness itself sends, for a test that wants
    /// to take one of these steps by hand.
    static std::vector<uint8_t> authChallengeBody(uint32_t serverSeed);
    static std::vector<uint8_t> charEnumBody(const PlayerSpec& player);
    /// The list of an account with several characters, for a relog.
    static std::vector<uint8_t> charEnumBody(const std::vector<PlayerSpec>& characters);
    static std::vector<uint8_t> loginVerifyWorldBody(const PlayerSpec& player);
    /// SMSG_UPDATE_OBJECT with one block creating an object of `type` with
    /// these update fields (by index in the active table): a unit or a player
    /// alive at the position, anything else with no movement block.
    static std::vector<uint8_t> createObjectBody(uint64_t guid, game::ObjectType type,
                                                 std::vector<std::pair<uint16_t, uint32_t>> fields,
                                                 float x = 0.0f, float y = 0.0f, float z = 0.0f);

    /// Update fields by index, as an SMSG_UPDATE_OBJECT block carries them.
    using Fields = std::map<uint16_t, uint32_t>;
    /// A guid in a pair of fields, low word first.
    static void putGuid(Fields& fields, uint16_t field, uint64_t guid);
    /// SMSG_UPDATE_OBJECT creating an item object, as 3.3.5a writes one: no
    /// movement, then its entry and stack count.
    static std::vector<uint8_t> createItemBody(uint64_t guid, uint32_t entry, uint32_t stack);
    /// SMSG_ITEM_QUERY_SINGLE_RESPONSE describing one item template, field for
    /// field as AzerothCore writes it: this display and inventory type, the
    /// rest a plain item with no stats, spells or sockets.
    static std::vector<uint8_t> itemTemplateBody(uint32_t entry, const std::string& name,
                                                 uint32_t displayInfoId, uint32_t inventoryType);
    /// SMSG_UPDATE_OBJECT with one VALUES block for an object.
    static std::vector<uint8_t> valuesBody(uint64_t guid, const Fields& fields);
    /// Put the player's own object in view - what the realm's fields for the
    /// character are applied to.
    void playerInView();

private:
    core::EventBus events_;
    core::CVarStore cvars_;
    std::unique_ptr<game::EmoteTable> emotes_;
    game::ExpansionRegistry expansions_;
    game::GameServices services_;
    std::unique_ptr<game::GameHandler> game_;
    FakeWorldConnection* connection_ = nullptr;  // owned by the game once made
    bool pingsAnswered_ = true;
    bool connectionsRefused_ = false;
    std::vector<AddonEvent> addonEvents_;
    PlayerSpec player_;
};

/// A body under construction, little-endian as the wire is.
class Body {
public:
    Body& u8(uint8_t v) { bytes_.push_back(v); return *this; }
    Body& u32(uint32_t v);
    Body& u64(uint64_t v);
    Body& f32(float v);
    /// A NUL-terminated string.
    Body& cstr(const std::string& s);
    Body& packedGuid(uint64_t guid);
    Body& zeros(size_t n) { bytes_.insert(bytes_.end(), n, 0); return *this; }
    [[nodiscard]] const std::vector<uint8_t>& bytes() const { return bytes_; }
    operator const std::vector<uint8_t>&() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

} // namespace wowee::test
