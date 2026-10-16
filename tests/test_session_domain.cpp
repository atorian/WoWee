#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/game_events.hpp"
#include "game/entity.hpp"
#include "game/movement_handler.hpp"
#include "game/movement_flags.hpp"
#include "game/entity_controller.hpp"
#include "game/update_field_table.hpp"
#include "core/world_transition_queue.hpp"

#include <algorithm>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

TEST_CASE("suspending client movement sends stop packets before controls are frozen", "[session][harness][loading]") {
    GameHarness h;
    h.enterWorld();
    h.game().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    h.game().sendMovement(Opcode::MSG_MOVE_START_STRAFE_LEFT);
    h.game().sendMovement(Opcode::MSG_MOVE_START_TURN_RIGHT);
    h.game().sendMovement(Opcode::MSG_MOVE_START_PITCH_UP);
    h.game().sendMovement(Opcode::MSG_MOVE_START_ASCEND);
    h.server().clearSent();
    h.game().getMovementHandler()->stopInputMovement();
    CHECK(h.sentOpcodes() == std::vector<wowee::game::LogicalOpcode>{Opcode::MSG_MOVE_STOP,
        Opcode::MSG_MOVE_STOP_STRAFE, Opcode::MSG_MOVE_STOP_TURN, Opcode::MSG_MOVE_STOP_PITCH, Opcode::MSG_MOVE_STOP_ASCEND});
    // The heartbeat the server reads after loading must carry the stopped state.
    h.game().sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    auto* heartbeat = h.lastSent(Opcode::MSG_MOVE_HEARTBEAT);
    REQUIRE(heartbeat);
    auto packet = *heartbeat;
    REQUIRE(packet.readPackedGuid() == h.player().guid);
    const uint32_t flags = packet.readUInt32();
    using F = wowee::game::MovementFlags;
    CHECK((flags & (static_cast<uint32_t>(F::FORWARD) | static_cast<uint32_t>(F::STRAFE_LEFT) |
                    static_cast<uint32_t>(F::TURN_RIGHT) | static_cast<uint32_t>(F::PITCH_UP) |
                    static_cast<uint32_t>(F::ASCENDING))) == 0);
    h.server().clearSent();
    h.game().getMovementHandler()->stopInputMovement();
    CHECK(h.sentOpcodes().empty());
}

TEST_CASE("network dispatch receives logout state and only Game advances its countdown",
          "[session][harness][frame]") {
    GameHarness h;
    h.enterWorld();
    h.game().requestLogout();
    h.receive(Opcode::SMSG_LOGOUT_RESPONSE, Body().u32(0).u8(0));
    h.game().updateNetwork(0.0f);
    REQUIRE(h.game().isLoggingOut());
    const float countdown = h.game().getLogoutCountdown();
    REQUIRE(countdown > 0);
    h.game().updateNetwork(3.0f);
    CHECK(h.game().getLogoutCountdown() == countdown);
    h.game().updateGame(3.0f);
    CHECK(h.game().getLogoutCountdown() == Catch::Approx(countdown - 3.0f));
}

TEST_CASE("logout completion announces the destination and preserves the session during pump",
          "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    const bool exiting = GENERATE(false, true);
    constexpr uint64_t creatureGuid = 0x1234;
    auto creature = std::make_shared<wowee::game::Unit>(creatureGuid);
    h.game().getEntityManager().addEntity(creatureGuid, creature);
    std::vector<ev::LogoutCompleted> completed;
    auto subscription = h.events().subscribe<ev::LogoutCompleted>([&](const auto& event) {
        completed.push_back(event);
        CHECK(h.game().getPlayerGuid() == h.player().guid);
        CHECK(h.game().isInWorld());
        CHECK(h.game().getEntityManager().getEntity(creatureGuid) == creature);
    });

    h.game().requestLogout(exiting);
    h.receive(Opcode::SMSG_LOGOUT_COMPLETE);
    h.pump();

    REQUIRE(completed.size() == 1);
    CHECK(completed.front().exiting == exiting);
    CHECK(h.game().getPlayerGuid() == h.player().guid);
    CHECK(h.game().isConnected());
    CHECK(h.game().isInWorld());
    CHECK(h.game().getEntityManager().getEntity(creatureGuid) == creature);

    // The client's command happens after pump, and only then releases state.
    h.game().returnToCharacterSelect();
    CHECK_FALSE(h.game().isInWorld());
    CHECK_FALSE(h.game().getEntityManager().hasEntity(creatureGuid));
    CHECK(h.game().isConnected());
    CHECK(h.countSent(Opcode::CMSG_CHAR_ENUM) == 1);
}

TEST_CASE("a closed connection announces disconnect once and leaves state for client cleanup",
          "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    auto creature = std::make_shared<wowee::game::Unit>(0x1234);
    h.game().getEntityManager().addEntity(creature->getGuid(), creature);
    std::vector<ev::Disconnected> disconnected;
    auto subscription = h.events().subscribe<ev::Disconnected>([&](const auto& event) {
        disconnected.push_back(event);
        CHECK(h.game().getPlayerGuid() == h.player().guid);
    });

    h.server().disconnect();
    h.pump();
    h.pump();
    REQUIRE(disconnected.size() == 1);
    CHECK(disconnected.front().characterGuid == h.player().guid);
    CHECK(h.game().getState() == wowee::game::WorldState::DISCONNECTED);
    CHECK(h.game().getPlayerGuid() == h.player().guid);
    CHECK(h.game().getEntityManager().getEntity(creature->getGuid()) == creature);
    h.game().disconnect();
    CHECK_FALSE(h.game().getEntityManager().hasEntity(creature->getGuid()));
    CHECK_FALSE(h.game().isConnected());
}

TEST_CASE("NEW_WORLD waits for the client boundary and holds subsequent packets",
          "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    wowee::core::WorldTransitionQueue transitions(h.events());
    auto creature = std::make_shared<wowee::game::Unit>(0x1234);
    h.game().getEntityManager().addEntity(creature->getGuid(), creature);
    auto despawns = std::make_shared<unsigned>(0);
    h.game().setCreatureDespawnCallback([despawns, creatureGuid = creature->getGuid()](uint64_t guid) {
        CHECK(guid == creatureGuid);
        ++*despawns;
    });
    h.receive(Opcode::SMSG_NEW_WORLD, Body().u32(1).f32(1000).f32(2000).f32(300).f32(1));
    h.receive(Opcode::SMSG_TIME_SYNC_REQ, Body().u32(91));
    h.game().update(1.0f / 60.0f);

    CHECK(h.game().getCurrentMapId() == h.player().mapId);
    CHECK(h.game().getEntityManager().getEntity(creature->getGuid()) == creature);
    CHECK(*despawns == 0);
    CHECK(h.countSent(Opcode::MSG_MOVE_WORLDPORT_ACK) == 0);
    CHECK(h.countSent(Opcode::CMSG_TIME_SYNC_RESP) == 0);

    auto request = transitions.takeNext();
    REQUIRE(request);
    REQUIRE(std::holds_alternative<ev::WorldTransferStarted>(*request));
    CHECK(std::get<ev::WorldTransferStarted>(*request).mapId == 1);
    h.game().getMovementHandler()->applyPendingWorldTransfer();

    CHECK(h.game().getCurrentMapId() == 1);
    CHECK_FALSE(h.game().getEntityManager().hasEntity(creature->getGuid()));
    CHECK(*despawns == 1);
    CHECK(h.countSent(Opcode::MSG_MOVE_WORLDPORT_ACK) == 1);
    request = transitions.takeNext();
    REQUIRE(request);
    REQUIRE(std::holds_alternative<ev::WorldEntered>(*request));
    const auto& entered = std::get<ev::WorldEntered>(*request);
    CHECK(entered.mapId == 1);
    CHECK(entered.x == 1000);
    CHECK(entered.y == 2000);
    CHECK(entered.z == 300);
    CHECK_FALSE(entered.isInitialEntry);
    h.pump();
    CHECK(h.countSent(Opcode::CMSG_TIME_SYNC_RESP) == 1);
}

TEST_CASE("logout supersedes queued world entry and a disconnect supersedes character select",
          "[session][harness]") {
    GameHarness h;
    wowee::core::WorldTransitionQueue transitions(h.events());
    h.enterWorld();
    h.receive(Opcode::SMSG_LOGOUT_COMPLETE);
    h.pump();
    auto request = transitions.takeNext();
    REQUIRE(request);
    REQUIRE(std::holds_alternative<wowee::core::WorldTransitionQueue::Leave>(*request));
    CHECK(std::get<wowee::core::WorldTransitionQueue::Leave>(*request).destination ==
          wowee::core::WorldTransitionQueue::Destination::CharacterSelect);
    CHECK(transitions.empty());

    h.receive(Opcode::SMSG_LOGOUT_COMPLETE);
    h.server().disconnect();
    h.pump();
    request = transitions.takeNext();
    REQUIRE(request);
    const auto& leave = std::get<wowee::core::WorldTransitionQueue::Leave>(*request);
    CHECK(leave.destination == wowee::core::WorldTransitionQueue::Destination::Reconnect);
    CHECK(leave.characterGuid == h.player().guid);
    CHECK(transitions.empty());
}

TEST_CASE("forced exit remains queued when another transition arrives", "[session][harness]") {
    GameHarness h;
    wowee::core::WorldTransitionQueue transitions(h.events());
    h.game().requestLogout(true); // no connection: local exit, no server completion
    transitions.leave(wowee::core::WorldTransitionQueue::Destination::Login);
    auto request = transitions.takeNext();
    REQUIRE(request);
    REQUIRE(std::holds_alternative<wowee::core::WorldTransitionQueue::Leave>(*request));
    CHECK(std::get<wowee::core::WorldTransitionQueue::Leave>(*request).destination ==
          wowee::core::WorldTransitionQueue::Destination::Exit);
    CHECK(transitions.empty());
}

TEST_CASE("a disconnect at the NEW_WORLD boundary cancels entry before client cleanup",
          "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    wowee::core::WorldTransitionQueue transitions(h.events());
    h.receive(Opcode::SMSG_NEW_WORLD, Body().u32(1).f32(1000).f32(2000).f32(300).f32(1));
    h.receive(Opcode::SMSG_TIME_SYNC_REQ, Body().u32(91));
    h.server().disconnect();
    h.game().update(1.0f / 60.0f);
    CHECK(h.game().getState() == wowee::game::WorldState::DISCONNECTED);
    CHECK(h.game().getCurrentMapId() == h.player().mapId);
    CHECK(h.game().getPlayerGuid() == h.player().guid);
    auto request = transitions.takeNext();
    REQUIRE(request);
    REQUIRE(std::holds_alternative<wowee::core::WorldTransitionQueue::Leave>(*request));
    CHECK(std::get<wowee::core::WorldTransitionQueue::Leave>(*request).destination ==
          wowee::core::WorldTransitionQueue::Destination::Reconnect);
    CHECK(transitions.empty());
    h.game().disconnect();
    CHECK_FALSE(h.game().getMovementHandler()->hasPendingWorldTransfer());
}

TEST_CASE("a malformed NEW_WORLD leaves the current map usable and does not block the stream",
          "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    wowee::core::WorldTransitionQueue transitions(h.events());
    h.receive(Opcode::SMSG_NEW_WORLD, Body().u32(1));
    h.receive(Opcode::SMSG_TIME_SYNC_REQ, Body().u32(91));
    h.pump();
    CHECK(h.game().getCurrentMapId() == h.player().mapId);
    CHECK(h.game().isInWorld());
    CHECK(h.countSent(Opcode::CMSG_TIME_SYNC_RESP) == 1);
    CHECK(transitions.empty());
}

TEST_CASE("login publishes the server destination for deferred world entry", "[session][harness]") {
    GameHarness h;
    std::vector<ev::WorldEntered> entered;
    auto subscription = h.events().subscribe<ev::WorldEntered>([&](const auto& event) {
        entered.push_back(event);
    });
    h.enterWorld();
    REQUIRE(entered.size() == 1);
    CHECK(entered.front().mapId == h.player().mapId);
    CHECK(entered.front().x == h.player().x);
    CHECK(entered.front().y == h.player().y);
    CHECK(entered.front().z == h.player().z);
    CHECK(entered.front().isInitialEntry);
}

TEST_CASE("transfer pending announces its map without destroying the current world",
          "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::WorldTransferStarted> transfers;
    auto subscription = h.events().subscribe<ev::WorldTransferStarted>([&](const auto& event) {
        transfers.push_back(event);
    });
    h.receive(Opcode::SMSG_TRANSFER_PENDING, Body().u32(1));
    h.pump();
    REQUIRE(transfers.size() == 1);
    CHECK(transfers.front().mapId == 1);
    CHECK(h.game().getCurrentMapId() == h.player().mapId);
    CHECK(h.game().getPlayerGuid() == h.player().guid);
}

TEST_CASE("a new map scope is shown every object already in view, once, without asking the server",
          "[session][harness][mapscope]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();

    auto creature = std::make_shared<Unit>(0x1001);
    creature->setDisplayId(4321);
    creature->setMaxHealth(100);
    creature->setHealth(100);
    creature->setNpcFlags(0x02);  // a quest giver: its status is not asked again
    creature->setPosition(10.0f, 20.0f, 30.0f, 1.5f);
    auto corpse = std::make_shared<Unit>(0x1002);
    corpse->setDisplayId(4322);
    corpse->setMaxHealth(100);
    corpse->setHealth(0);
    auto invisible = std::make_shared<Unit>(0x1003);  // no display: never drawn
    auto rider = std::make_shared<Player>(0x1004);
    rider->setDisplayId(49);
    rider->setMountDisplayId(14337);
    auto door = std::make_shared<GameObject>(0x1005);
    door->setEntry(77);
    door->setDisplayId(411);
    door->setField(fieldIndex(UF::GAMEOBJECT_BYTES_1), 0);  // open
    for (const auto& entity : std::initializer_list<std::shared_ptr<Entity>>{
             creature, corpse, invisible, rider, door})
        h.game().getEntityManager().addEntity(entity->getGuid(), entity);

    std::vector<uint64_t> creatures, deaths;
    std::vector<std::pair<uint64_t, uint32_t>> mounts;
    std::vector<std::pair<uint64_t, uint8_t>> doorStates;
    std::vector<uint64_t> objects;
    h.game().setCreatureSpawnCallback([&](uint64_t guid, uint32_t displayId, float x, float y, float z,
                                          float orientation, float) {
        creatures.push_back(guid);
        if (guid == creature->getGuid()) {
            CHECK(displayId == 4321);
            CHECK(x == 10.0f);
            CHECK(y == 20.0f);
            CHECK(z == 30.0f);
            CHECK(orientation == 1.5f);
        }
    });
    h.game().setNpcDeathCallback([&](uint64_t guid) { deaths.push_back(guid); });
    h.game().setOtherPlayerMountCallback([&](uint64_t guid, uint32_t display) { mounts.emplace_back(guid, display); });
    h.game().setGameObjectStateCallback([&](uint64_t guid, uint8_t state) { doorStates.emplace_back(guid, state); });
    h.game().setGameObjectSpawnCallback([&](uint64_t guid, uint32_t entry, uint32_t displayId,
                                            float, float, float, float, float) {
        objects.push_back(guid);
        CHECK(entry == 77);
        CHECK(displayId == 411);
    });
    h.server().clearSent();

    h.game().getEntityController()->announceObjectsInView();

    std::sort(creatures.begin(), creatures.end());
    CHECK(creatures == std::vector<uint64_t>{creature->getGuid(), corpse->getGuid()});
    CHECK(deaths == std::vector<uint64_t>{corpse->getGuid()});
    CHECK(mounts == std::vector<std::pair<uint64_t, uint32_t>>{{rider->getGuid(), 14337}});
    CHECK(doorStates == std::vector<std::pair<uint64_t, uint8_t>>{{door->getGuid(), 0}});
    CHECK(objects == std::vector<uint64_t>{door->getGuid()});
    CHECK(h.sentOpcodes().empty());
}

TEST_CASE("an empty view announces nothing, and the local player is never presented as another",
          "[session][harness][mapscope]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    auto self = std::make_shared<Player>(h.player().guid);
    self->setDisplayId(49);
    h.game().getEntityManager().addEntity(self->getGuid(), self);
    unsigned spawns = 0;
    h.game().setPlayerSpawnCallback([&](uint64_t, uint32_t, uint8_t, uint8_t, uint32_t, uint8_t,
                                        float, float, float, float) { ++spawns; });
    h.game().setCreatureSpawnCallback([&](uint64_t, uint32_t, float, float, float, float, float) { ++spawns; });
    h.game().getEntityController()->announceObjectsInView();
    CHECK(spawns == 0);
}

TEST_CASE("game notifications wait for the interface and own their arguments", "[session][harness][addonqueue]") {
    wowee::test::GameHarness h;
    h.enterWorld();
    std::vector<std::string> delivered;
    h.game().setAddonEventCallback([&](const auto& event, const auto& args) {
        delivered.push_back(event + ":" + args.at(0));
    });
    std::string argument = "player";
    h.game().fireAddonEvent("UNIT_HEALTH", {argument});
    argument = "target";
    h.game().fireAddonEvent("UNIT_HEALTH", {argument});
    CHECK(delivered.empty());
    h.pump();
    CHECK(delivered == std::vector<std::string>{"UNIT_HEALTH:player", "UNIT_HEALTH:target"});
}

// ---- A lost world connection, and logging in again --------------------------

namespace {

/// Every Disconnected the game announces, while it lives.
struct DisconnectsHeard {
    explicit DisconnectsHeard(GameHarness& h)
        : sub(h.events().subscribe<ev::Disconnected>(
              [this](const ev::Disconnected& e) { seen.push_back(e); })) {}
    std::vector<ev::Disconnected> seen;
    wowee::core::EventBus::Subscription sub;
};

/// What a client does when the connection is lost: drop what is left of it
/// and log the same character in again with the last login, on a new
/// connection the test plays the server of.
void reconnect(GameHarness& h) {
    h.game().disconnect();
    REQUIRE_FALSE(h.hasServer());
    REQUIRE(h.game().connect(h.game().lastWorldLogin()));
    h.answerLogin(h.player());
    h.game().selectCharacter(h.player().guid);
    h.answerPlayerLogin(h.player());
}

}  // namespace

TEST_CASE("a world server silent past an unanswered ping is lost, once", "[session][harness][reconnect]") {
    GameHarness h;
    h.enterWorld();
    DisconnectsHeard heard(h);
    h.setPingsAnswered(false);

    // Ten quiet seconds: asked about, not yet given up on.
    h.advance(10.5f);
    CHECK(h.countSent(Opcode::CMSG_PING) == 1);
    CHECK(heard.seen.empty());

    // Ten more with no answer: the connection is lost, though the socket
    // never said so.
    h.advance(10.0f);
    REQUIRE(heard.seen.size() == 1);
    CHECK(heard.seen[0].characterGuid == h.player().guid);
    CHECK(h.game().getState() == wowee::game::WorldState::DISCONNECTED);
    CHECK(h.game().worldConnectionLost());
    h.advance(30.0f);
    CHECK(heard.seen.size() == 1);
}

TEST_CASE("a quiet server that answers the ping, a long frame and a map change are not silence",
          "[session][harness][reconnect]") {
    GameHarness h;
    h.enterWorld();
    DisconnectsHeard heard(h);

    SECTION("a quiet zone: the server answers when asked") {
        h.advance(60.0f);
        CHECK(h.countSent(Opcode::CMSG_PING) >= 1);
    }
    SECTION("a frame held for a long load is the client not listening") {
        h.setPingsAnswered(false);
        for (int i = 0; i < 3; ++i) h.game().update(30.0f);
    }
    SECTION("between NEW_WORLD and the client's acknowledgement the server waits for us") {
        h.setPingsAnswered(false);
        h.receive(Opcode::SMSG_NEW_WORLD, Body().u32(1).f32(1.0f).f32(2.0f).f32(3.0f).f32(0.0f));
        h.pump();
        REQUIRE(h.game().getMovementHandler()->hasPendingWorldTransfer());
        h.advance(40.0f);
    }
    CHECK(heard.seen.empty());
    CHECK_FALSE(h.game().worldConnectionLost());
}

TEST_CASE("while the world connection is lost, what needs the server is refused at once and not kept",
          "[session][harness][reconnect]") {
    GameHarness h;
    h.enterWorld();
    h.server().disconnect();
    h.pump();
    REQUIRE(h.game().worldConnectionLost());
    h.clearRecords();

    h.game().castSpell(133, 0);
    h.game().sendChatMessage(wowee::game::ChatType::SAY, "anyone there?", "");
    h.game().lootTarget(0xF130000001000001ull);
    h.game().interactWithNpc(0xF130000001000002ull);
    h.pump();
    // Told, each time.
    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    CHECK(errors.size() == 4);
    for (const auto& e : errors) CHECK(e.args.back() == "No connection to the server.");

    // Back in: none of it goes out after all.
    reconnect(h);
    h.advance(1.0f);
    CHECK_FALSE(h.game().worldConnectionLost());
    CHECK(h.countSent(Opcode::CMSG_CAST_SPELL) == 0);
    CHECK(h.countSent(Opcode::CMSG_MESSAGECHAT) == 0);
    CHECK(h.countSent(Opcode::CMSG_LOOT) == 0);
    CHECK(h.countSent(Opcode::CMSG_GOSSIP_HELLO) == 0);
    // And a command once in again goes.
    h.game().castSpell(133, 0);
    CHECK(h.countSent(Opcode::CMSG_CAST_SPELL) == 1);
}

TEST_CASE("logging in again after a lost connection keeps nothing of the last one",
          "[session][harness][reconnect]") {
    using wowee::game::ObjectType;
    using wowee::game::UF;
    using wowee::game::fieldIndex;
    constexpr uint32_t kFireball = 133;
    constexpr uint64_t kJaina = 0x21;
    constexpr uint64_t kItem = 0x4000000000000101ull;

    GameHarness h;
    h.enterWorld();
    const uint64_t me = h.player().guid;
    // A cooldown, an aura, a party and a hearthstone in the backpack.
    h.receive(Opcode::SMSG_SPELL_COOLDOWN, Body().u64(me).u8(0).u32(kFireball).u32(60000));
    h.receive(Opcode::SMSG_AURA_UPDATE,
              Body().packedGuid(me).u8(0).u32(1459).u8(0x08 /* no caster */).u8(10).u8(0));
    {
        Body party;
        party.u8(0).u8(0).u8(0).u8(0).u64(0x1234).u32(1).u32(1);
        party.cstr("Jaina").u64(kJaina).u8(1).u8(0).u8(0).u8(0);
        party.u64(me).u8(2).u64(me).u8(2).u8(1).u8(0).u8(0);
        h.receive(Opcode::SMSG_GROUP_LIST, party);
    }
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kItem, ObjectType::ITEM,
                                            {{fieldIndex(UF::OBJECT_FIELD_ENTRY), 6948},
                                             {fieldIndex(UF::ITEM_FIELD_STACK_COUNT), 1}}));
    const uint16_t pack = fieldIndex(UF::PLAYER_FIELD_PACK_SLOT_1);
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(me, ObjectType::PLAYER,
                                            {{fieldIndex(UF::UNIT_FIELD_HEALTH), 100},
                                             {fieldIndex(UF::UNIT_FIELD_MAXHEALTH), 100},
                                             {pack, static_cast<uint32_t>(kItem)},
                                             {static_cast<uint16_t>(pack + 1),
                                              static_cast<uint32_t>(kItem >> 32)}},
                                            h.player().x, h.player().y, h.player().z));
    h.pump();
    REQUIRE(h.game().getSpellCooldown(kFireball) > 0.0f);
    REQUIRE_FALSE(h.game().getPlayerAuras().empty());
    REQUIRE(h.game().isInGroup());
    REQUIRE_FALSE(h.game().getInventory().getBackpackSlot(0).empty());

    h.server().disconnect();
    h.pump();
    reconnect(h);

    // The new session is the server's to fill; nothing of the last is left.
    CHECK(h.game().getSpellCooldown(kFireball) == 0.0f);
    CHECK(h.game().getPlayerAuras().empty());
    CHECK_FALSE(h.game().isInGroup());
    CHECK(h.game().getInventory().getBackpackSlot(0).empty());
    CHECK_FALSE(h.game().getEntityManager().hasEntity(kItem));
}
