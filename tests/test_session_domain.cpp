#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/game_events.hpp"
#include "game/entity.hpp"
#include "game/movement_handler.hpp"
#include "core/world_transition_queue.hpp"

#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

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
