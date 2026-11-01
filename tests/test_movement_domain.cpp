// Movement, end to end: what the server says of where things are and go,
// and what the rest of the client is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "core/coordinates.hpp"
#include "game/entity.hpp"
#include "game/world_handler.hpp"
#include "game/game_events.hpp"
#include "game/transport_manager.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"

#include <glm/vec3.hpp>

#include <memory>
#include <string>
#include <vector>

using wowee::test::GameHarness;
namespace ev = wowee::game::events;

namespace {

uint16_t uf(wowee::game::UF field) { return wowee::game::fieldIndex(field); }

/// The realm puts a creature in view: SMSG_UPDATE_OBJECT creating a unit, alive.
void creatureInView(GameHarness& h, uint64_t guid) {
    h.receive(wowee::game::Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(guid, wowee::game::ObjectType::UNIT,
                                            {{uf(wowee::game::UF::OBJECT_FIELD_ENTRY), 299},
                                             {uf(wowee::game::UF::UNIT_FIELD_HEALTH), 100}}));
    h.pump();
}

}  // namespace

TEST_CASE("following stops on the next frame once the followed player leaves view", "[movement][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    const auto& me = h.game().getMovementInfo();
    // Anna, three yards east, and her name as the server answers the query
    // her arrival asks.
    const glm::vec3 at = wowee::core::coords::canonicalToServer(glm::vec3(me.x + 3.0f, me.y, me.z));
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(0xB001, ObjectType::PLAYER, {{uf(UF::UNIT_FIELD_HEALTH), 100}},
                                            at.x, at.y, at.z));
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, GameHarness::nameResponseBody(0xB001, "Anna"));
    h.pump();
    std::vector<bool> following;
    auto sub = h.events().subscribe<ev::AutoFollowChanged>(
        [&](const ev::AutoFollowChanged& e) { following.push_back(e.renderPos != nullptr); });

    h.game().combat().setTarget(0xB001);
    h.game().movement().followTarget();
    h.pump();
    REQUIRE(following == std::vector<bool>{true});
    REQUIRE(h.addonEvents("AUTOFOLLOW_BEGIN").size() == 1);
    CHECK(h.addonEvents("AUTOFOLLOW_BEGIN")[0].args.at(0) == "Anna");

    h.receive(Opcode::SMSG_DESTROY_OBJECT, wowee::test::Body().u64(0xB001).u8(0));
    h.pump();
    h.pump();
    CHECK(following == std::vector<bool>{true, false});
    CHECK(h.addonEvents("AUTOFOLLOW_END").size() == 1);
}

namespace {
using wowee::game::UpdateBlock;
using wowee::game::UpdateObjectData;
using wowee::game::ObjectType;
using wowee::game::UpdateType;

constexpr uint64_t kBoat = 0x1F10000000000042ull;

/// The boat comes into view as a transport: a game object whose create block
/// carries UPDATEFLAG_TRANSPORT.
void boatArrives(GameHarness& h) {
    UpdateBlock boat{};
    boat.guid = kBoat;
    boat.objectType = ObjectType::GAMEOBJECT;
    boat.updateType = UpdateType::CREATE_OBJECT;
    boat.updateFlags = 0x0002;
    boat.hasMovement = true;
    boat.x = 100.0f; boat.y = 200.0f; boat.z = 0.0f;
    UpdateObjectData batch{};
    batch.blocks.push_back(boat);
    h.game().world().applyRecordedObjects(batch);
}

/// The server moves the player: aboard `transport` at a server-space offset,
/// or (transport 0) standing on their own.
void playerMoves(GameHarness& h, uint64_t transport, float ox = 0, float oy = 0, float oz = 0) {
    UpdateBlock self{};
    self.guid = h.player().guid;
    self.objectType = ObjectType::PLAYER;
    self.updateType = UpdateType::MOVEMENT;
    self.hasMovement = true;
    self.x = 100.0f; self.y = 200.0f; self.z = 1.0f;
    self.onTransport = transport != 0;
    self.transportGuid = transport;
    self.transportX = ox; self.transportY = oy; self.transportZ = oz;
    UpdateObjectData batch{};
    batch.blocks.push_back(self);
    h.game().world().applyRecordedObjects(batch);
}

void enterWorldAsEntity(GameHarness& h) {
    h.enterWorld();
    h.playerInView();
}
}  // namespace

TEST_CASE("the player rides the transport the server names, and steps off when it names none",
          "[movement][transport][harness]") {
    GameHarness h;
    enterWorldAsEntity(h);
    boatArrives(h);

    // Server offsets are x west, y north; on a transport this client has not
    // placed, canonical is the swap.
    playerMoves(h, kBoat, 1.0f, 2.0f, 3.0f);
    REQUIRE(h.game().movement().isOnTransport());
    CHECK(h.game().movement().getPlayerTransportGuid() == kBoat);
    CHECK(h.game().movement().getPlayerTransportOffset() == glm::vec3(2.0f, 1.0f, 3.0f));
    CHECK(h.game().getMovementInfo().transportGuid == kBoat);

    playerMoves(h, 0);
    CHECK_FALSE(h.game().movement().isOnTransport());
    CHECK(h.game().movement().getPlayerTransportOffset() == glm::vec3(0.0f));
    CHECK(h.game().getMovementInfo().transportGuid == 0);
}

// Transport guids are reused across maps - an Undercity elevator's becomes a
// Deeprun tram's - so the ride ends with the world it was in. The game knows
// when it leaves one; the client's scene ending is not what decides it.
TEST_CASE("the player's ride ends when the server puts the player in another world",
          "[movement][transport][harness]") {
    using wowee::game::Opcode;
    GameHarness h;
    enterWorldAsEntity(h);
    boatArrives(h);
    playerMoves(h, kBoat, 1.0f, 2.0f, 3.0f);
    REQUIRE(h.game().movement().isOnTransport());

    wowee::test::PlayerSpec elsewhere = h.player();
    elsewhere.mapId = 1;  // Kalimdor
    elsewhere.x = 1629.0f;
    elsewhere.y = -4373.0f;
    elsewhere.z = 31.0f;
    h.receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, GameHarness::loginVerifyWorldBody(elsewhere));
    h.pump();

    CHECK_FALSE(h.game().movement().isOnTransport());
    CHECK(h.game().getMovementInfo().transportGuid == 0);
}

TEST_CASE("the player's ride ends when the server puts the player elsewhere on the same map",
          "[movement][transport][harness]") {
    using wowee::game::Opcode;
    GameHarness h;
    enterWorldAsEntity(h);
    boatArrives(h);
    playerMoves(h, kBoat, 1.0f, 2.0f, 3.0f);
    REQUIRE(h.game().movement().isOnTransport());

    // A teleport across the continent: the boat is still in view until the
    // server says it is not, and the player is not on it.
    wowee::test::PlayerSpec across = h.player();
    across.x += 2000.0f;
    across.y += 1500.0f;
    h.receive(Opcode::SMSG_LOGIN_VERIFY_WORLD, GameHarness::loginVerifyWorldBody(across));
    h.pump();

    CHECK_FALSE(h.game().movement().isOnTransport());
    CHECK(h.game().getMovementInfo().transportGuid == 0);
}

TEST_CASE("the player's ride ends with the character's session", "[movement][transport][harness][relog]") {
    GameHarness h;
    enterWorldAsEntity(h);
    boatArrives(h);
    playerMoves(h, kBoat, 1.0f, 2.0f, 3.0f);
    REQUIRE(h.game().movement().isOnTransport());

    wowee::test::PlayerSpec next;
    next.guid = 0x91;
    next.name = "Next";
    h.relogAs(next);

    CHECK_FALSE(h.game().movement().isOnTransport());
    CHECK(h.game().getMovementInfo().transportGuid == 0);
}

TEST_CASE("a transport the client has not seen yet does not carry the player", "[movement][transport][harness]") {
    GameHarness h;
    enterWorldAsEntity(h);

    playerMoves(h, kBoat, 1.0f, 2.0f, 3.0f);
    CHECK_FALSE(h.game().movement().isOnTransport());
}

TEST_CASE("a transport just stepped off survives being destroyed for a moment, and not after",
          "[movement][transport][harness]") {
    using wowee::game::Opcode;
    using wowee::test::Body;
    GameHarness h;
    enterWorldAsEntity(h);
    boatArrives(h);
    playerMoves(h, kBoat, 1.0f, 2.0f, 3.0f);
    playerMoves(h, 0);

    // Within the hold the server's destroy is the transport flapping out of
    // range under a player who has only just left it.
    h.receive(Opcode::SMSG_DESTROY_OBJECT, Body().u64(kBoat).u8(0));
    h.pump();
    CHECK(h.game().getEntityManager().hasEntity(kBoat));

    // The hold runs eight seconds from boarding, and at least a second and a
    // half from stepping off.
    h.game().update(9.0f);
    h.receive(Opcode::SMSG_DESTROY_OBJECT, Body().u64(kBoat).u8(0));
    h.pump();
    CHECK_FALSE(h.game().getEntityManager().hasEntity(kBoat));
}

TEST_CASE("arriving on a new map ends what the old one left: offers, world states, units in view",
          "[movement][harness]") {
    using wowee::game::Opcode;
    using wowee::test::Body;
    GameHarness h;
    h.enterWorld();
    const uint32_t oldMap = h.game().getCurrentMapId();
    const uint32_t newMap = oldMap + 1;
    creatureInView(h, 0xC001);
    // A resurrection offered, and a battleground score showing.
    h.receive(Opcode::SMSG_RESURRECT_REQUEST, Body().u64(0x777).u32(5).cstr("Anna").u8(0).u8(0));
    h.receive(Opcode::SMSG_UPDATE_WORLD_STATE, Body().u32(1581).u32(42));
    h.pump();
    REQUIRE(h.game().combat().resurrectOfferStanding());
    REQUIRE(h.game().getWorldState(1581));

    h.receive(Opcode::SMSG_NEW_WORLD, Body().u32(newMap).f32(10.0f).f32(20.0f).f32(30.0f).f32(0.0f));
    h.pump();
    h.game().movement().applyPendingWorldTransfer();

    CHECK(h.game().getCurrentMapId() == newMap);
    CHECK_FALSE(h.game().combat().resurrectOfferStanding());
    CHECK_FALSE(h.game().getWorldState(1581));
    CHECK(h.game().getWorldStateMapId() == newMap);
    CHECK_FALSE(h.game().getEntityManager().hasEntity(0xC001));
    CHECK(h.countSent(Opcode::MSG_MOVE_WORLDPORT_ACK) == 1);
}

TEST_CASE("a resurrection on the same map brings the player back where they are, keeping the world",
          "[movement][harness]") {
    using wowee::game::Opcode;
    using wowee::test::Body;
    GameHarness h;
    h.enterWorld();
    const uint32_t here = h.game().getCurrentMapId();
    creatureInView(h, 0xC002);
    h.receive(Opcode::SMSG_FORCED_DEATH_UPDATE, Body());
    h.receive(Opcode::SMSG_SPIRIT_HEALER_CONFIRM, Body().u64(0xF130000000001234ull));
    h.pump();
    h.game().combat().acceptResurrect();
    REQUIRE(h.game().combat().resurrectPending());

    h.receive(Opcode::SMSG_NEW_WORLD, Body().u32(here).f32(10.0f).f32(20.0f).f32(30.0f).f32(0.0f));
    h.pump();
    h.game().movement().applyPendingWorldTransfer();

    CHECK_FALSE(h.game().combat().isPlayerDead());
    CHECK_FALSE(h.game().combat().resurrectPending());
    CHECK(h.game().getEntityManager().hasEntity(0xC002));
    CHECK(h.countSent(Opcode::MSG_MOVE_WORLDPORT_ACK) == 1);
}

TEST_CASE("the server's position wins when it puts the player far from where the client thinks",
          "[movement][harness]") {
    GameHarness h;
    enterWorldAsEntity(h);
    std::vector<ev::PlayerPositionCorrected> corrected;
    auto sub = h.events().subscribe<ev::PlayerPositionCorrected>(
        [&](const ev::PlayerPositionCorrected& e) { corrected.push_back(e); });
    auto serverPuts = [&](float canonicalX, float canonicalY, float z) {
        UpdateBlock self{};
        self.guid = h.player().guid;
        self.objectType = ObjectType::PLAYER;
        self.updateType = UpdateType::MOVEMENT;
        self.hasMovement = true;
        // Server x is canonical y (west), server y canonical x (north).
        self.x = canonicalY; self.y = canonicalX; self.z = z;
        UpdateObjectData batch{};
        batch.blocks.push_back(self);
        h.game().world().applyRecordedObjects(batch);
    };
    const auto me = h.game().getMovementInfo();

    // A step away is lag, and the client's own movement stands.
    serverPuts(me.x + 3.0f, me.y, me.z);
    CHECK(corrected.empty());

    // Two hundred yards is not lag.
    serverPuts(me.x + 200.0f, me.y, me.z);
    REQUIRE(corrected.size() == 1);
    CHECK(corrected[0].x == me.x + 200.0f);
    CHECK(corrected[0].y == me.y);
    CHECK(h.game().getMovementInfo().x == me.x + 200.0f);
}

namespace {

/// SMSG_SHOWTAXINODES: window, the flight master, the nearest node, then the
/// twelve words of the known-node mask (node n is bit n - 1).
std::vector<uint8_t> showTaxiNodes(uint32_t nearest, uint32_t mask0) {
    wowee::test::Body b;
    b.u32(1).u64(0xF130000000003003ull).u32(nearest).u32(mask0);
    for (int i = 1; i < 12; ++i) b.u32(0);
    return b.bytes();
}

}  // namespace

TEST_CASE("the flight map's nodes, routes and prices are TaxiNodes.dbc's and TaxiPath.dbc's",
          "[movement][harness][tables]") {
    using wowee::game::Opcode;
    GameHarness h;
    // ID, map, x, y, z, name; the mounts at 22 and 23.
    h.dbc("TaxiNodes.dbc", 24, {{{0, 2}, {1, 0}, {5, "Stormwind"}},
                                {{0, 4}, {1, 0}, {5, "Sentinel Hill"}}});
    // ID, from, to, cost.
    h.dbc("TaxiPath.dbc", 4, {{{0, 1}, {1, 2}, {2, 4}, {3, 50}}});
    h.enterWorld();

    h.receive(Opcode::SMSG_SHOWTAXINODES, showTaxiNodes(2, 0b0010));  // knows Stormwind
    h.pump();
    const auto& nodes = h.game().movement().getTaxiNodes();
    REQUIRE(nodes.count(4) == 1);
    CHECK(nodes.at(4).name == "Sentinel Hill");
    CHECK_FALSE(h.game().movement().hasTaxiRouteTo(4));  // not discovered yet

    h.receive(Opcode::SMSG_SHOWTAXINODES, showTaxiNodes(2, 0b1010));  // and Sentinel Hill
    h.pump();
    CHECK(h.game().movement().getTaxiCostTo(4) == 50);
    const auto& history = h.game().chat().getChatHistory();
    REQUIRE_FALSE(history.empty());
    CHECK(history.back().message == "Discovered flight path: Sentinel Hill");
}

TEST_CASE("walking into an AreaTrigger.dbc sphere tells the server which trigger",
          "[movement][harness][tables]") {
    using wowee::game::Opcode;
    GameHarness h;
    // ID, map, then the centre in the server's axes (x, y), z, and a radius.
    h.dbc("AreaTrigger.dbc", 10, {{{0, 45}, {1, 0}, {2, 2000.0f}, {3, 1000.0f}, {4, 50.0f},
                                   {5, 5.0f}}});
    h.enterWorld();
    h.advance(12.0f);  // past the hold every arrival puts on triggers
    REQUIRE(h.countSent(Opcode::CMSG_AREATRIGGER) == 0);

    h.game().movement().setPosition(1000.0f, 2000.0f, 50.0f);
    h.advance(1.0f);

    auto* sent = h.lastSent(Opcode::CMSG_AREATRIGGER);
    REQUIRE(sent != nullptr);
    CHECK(sent->readUInt32() == 45);
}

TEST_CASE("a ship sails the path TransportAnimation.dbc gives its entry, and a boat its TaxiPathNode.dbc path",
          "[movement][transport][harness][tables]") {
    GameHarness h;
    // ID, transport entry, time, x, y, z.
    h.dbc("TransportAnimation.dbc", 7, {
        {{0, 1}, {1, 20808}, {2, 0}, {3, 0.0f}, {4, 0.0f}, {5, 0.0f}},
        {{0, 2}, {1, 20808}, {2, 10000}, {3, 100.0f}, {4, 0.0f}, {5, 0.0f}},
        {{0, 3}, {1, 20808}, {2, 20000}, {3, 100.0f}, {4, 100.0f}, {5, 0.0f}},
    });
    // ID, path, index, map, x, y, z, flags, delay.
    h.dbc("TaxiPathNode.dbc", 9, {
        {{0, 1}, {1, 99}, {2, 0}, {3, 0}, {4, 0.0f}, {5, 0.0f}, {6, 0.0f}},
        {{0, 2}, {1, 99}, {2, 1}, {3, 0}, {4, 500.0f}, {5, 0.0f}, {6, 0.0f}},
    });
    h.enterWorld();
    auto& transports = *h.game().getTransportManager();

    transports.loadTransportAnimationDBC(h.gameData());
    transports.loadTaxiPathNodeDBC(h.gameData());

    CHECK(transports.hasPathForEntry(20808));
    CHECK_FALSE(transports.hasPathForEntry(20809));
    CHECK(transports.hasTaxiPath(99));
    CHECK_FALSE(transports.hasTaxiPath(98));
}

namespace {
uint16_t mountField() {
    return static_cast<uint16_t>(wowee::game::fieldIndex(wowee::game::UF::UNIT_FIELD_MOUNTDISPLAYID));
}
}  // namespace

TEST_CASE("the mount field puts the player on the mount just cast, and clearing it takes them off",
          "[movement][mount][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    std::vector<uint32_t> mounts;
    auto sub = h.events().subscribe<ev::PlayerMountChanged>(
        [&](const ev::PlayerMountChanged& e) { mounts.push_back(e.mountDisplayId); });
    h.game().spells().noteGroundCastSpell(458);   // Brown Horse
    h.clearRecords();

    h.playerFields({{mountField(), 14338}});
    CHECK(h.game().movement().isMounted());
    CHECK(h.game().movement().mountDisplayId() == 14338);
    CHECK(h.game().movement().mountAuraSpellId() == 458);
    CHECK(h.addonEvents("UNIT_MODEL_CHANGED").size() == 1);

    h.playerFields({{mountField(), 0}});
    CHECK_FALSE(h.game().movement().isMounted());
    CHECK(h.game().movement().mountAuraSpellId() == 0);
    CHECK(mounts == std::vector<uint32_t>{14338, 0});
}

TEST_CASE("a dismount for someone else is theirs, and one the mount field contradicts is not ours",
          "[movement][mount][harness]") {
    using wowee::game::Opcode;
    using wowee::test::Body;
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    std::vector<uint64_t> others;
    auto sub = h.events().subscribe<ev::OtherPlayerMounted>(
        [&](const ev::OtherPlayerMounted& e) { if (e.mountDisplayId == 0) others.push_back(e.guid); });
    h.playerFields({{mountField(), 14338}});
    std::vector<uint32_t> mounts;
    auto mine = h.events().subscribe<ev::PlayerMountChanged>(
        [&](const ev::PlayerMountChanged& e) { mounts.push_back(e.mountDisplayId); });

    h.receive(Opcode::SMSG_DISMOUNT, Body().packedGuid(0x77).bytes());
    h.pump();
    CHECK(others == std::vector<uint64_t>{0x77});
    CHECK(h.game().movement().isMounted());

    // Ours, while the field still says mounted: a transient the realm sends
    // mid-fight, not a dismount.
    h.receive(Opcode::SMSG_DISMOUNT, Body().packedGuid(h.player().guid).bytes());
    h.pump();
    CHECK(h.game().movement().isMounted());
    CHECK(mounts.empty());   // not even for a frame

    h.playerFields({{mountField(), 0}});
    CHECK_FALSE(h.game().movement().isMounted());
    CHECK(mounts == std::vector<uint32_t>{0});
}

TEST_CASE("a refused dismount is said once", "[movement][mount][harness]") {
    using wowee::game::Opcode;
    using wowee::test::Body;
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_DISMOUNTRESULT, Body().u32(1).bytes());
    h.pump();

    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args == std::vector<std::string>{"Cannot dismount here."});
}

TEST_CASE("the next character is not on the last one's mount", "[movement][mount][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.game().spells().noteGroundCastSpell(458);
    h.playerFields({{mountField(), 14338}});
    REQUIRE(h.game().movement().mountAuraSpellId() == 458);

    wowee::test::PlayerSpec bob;
    bob.guid = 0x11;
    bob.name = "Bob";
    h.relogAs(bob);
    CHECK_FALSE(h.game().movement().isMounted());
    CHECK(h.game().movement().mountAuraSpellId() == 0);
}

// Server commands through the real game; ACK values verified against build 12340.
TEST_CASE("forced movement flags acknowledge the original command value", "[movement][harness]") {
    using wowee::game::Opcode;
    using wowee::test::Body;
    struct Command { Opcode enable, disable, ack; bool hasValue; };
    const Command commands[] = {
        {Opcode::SMSG_MOVE_SET_HOVER, Opcode::SMSG_MOVE_UNSET_HOVER, Opcode::CMSG_MOVE_HOVER_ACK, true},
        {Opcode::SMSG_MOVE_FEATHER_FALL, Opcode::SMSG_MOVE_NORMAL_FALL, Opcode::CMSG_MOVE_FEATHER_FALL_ACK, true},
        {Opcode::SMSG_MOVE_WATER_WALK, Opcode::SMSG_MOVE_LAND_WALK, Opcode::CMSG_MOVE_WATER_WALK_ACK, true},
        {Opcode::SMSG_MOVE_SET_CAN_FLY, Opcode::SMSG_MOVE_UNSET_CAN_FLY, Opcode::CMSG_MOVE_SET_CAN_FLY_ACK, true},
        {Opcode::SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY, Opcode::SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY,
            Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, true},
        {Opcode::SMSG_MOVE_GRAVITY_DISABLE, Opcode::SMSG_MOVE_GRAVITY_ENABLE, Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK, false}
    };
    GameHarness h;
    h.enterWorld();
    uint32_t counter = 0x10203040;
    for (const auto& command : commands) {
        for (bool enabled : {true, false}) {
            CAPTURE(static_cast<unsigned>(command.ack), enabled);
            h.clearRecords();
            h.receive(enabled ? command.enable : command.disable, Body().packedGuid(h.player().guid).u32(++counter));
            h.pump();
            const auto ackOpcode = !command.hasValue && !enabled ? Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK : command.ack;
            auto* sent = h.lastSent(ackOpcode);
            REQUIRE(sent != nullptr);
            CHECK(h.countSent(ackOpcode) == 1);
            CHECK(sent->readPackedGuid() == h.player().guid);
            CHECK(sent->readUInt32() == counter);
            wowee::game::MovementInfo status;
            REQUIRE(wowee::game::MovementPacket::readMovementPayload(*sent, status));
            CHECK(sent->getRemainingSize() == (command.hasValue ? 4 : 0));
            if (command.hasValue && sent->hasRemaining(4)) CHECK(sent->readFloat() == (enabled ? 1.0f : 0.0f));
            CHECK(sent->getRemainingSize() == 0);
        }
    }
}
