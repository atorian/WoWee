#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/game_events.hpp"
#include "group/services/group.hpp"
#include "game/entity.hpp"
#include "game/movement_handler.hpp"
#include "game/movement_flags.hpp"
#include "game/world_handler.hpp"
#include "game/update_field_table.hpp"
#include "core/world_transition_queue.hpp"

#include <algorithm>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

// What the server tells one character, and what the player sets for it, is
// that character's: the next one logged in on the same connection starts
// with none of it, whether or not the server sends it again.
TEST_CASE("the next character logged in keeps nothing of the last one's", "[session][harness][relog]") {
    using wowee::test::PlayerSpec;
    PlayerSpec alice;
    alice.guid = 0x81;
    alice.name = "Alice";
    alice.classId = 6;  // a death knight, for the runes
    PlayerSpec bob;
    bob.guid = 0x82;
    bob.name = "Bob";
    constexpr uint64_t kInnkeeper = 0xF130000000000301ull;
    constexpr uint64_t kTrainer = 0xF130000000000302ull;
    constexpr uint64_t kBoar = 0xF130000000000303ull;

    GameHarness h;
    h.enterWorld(alice);
    // Home in Goldshire, and an innkeeper asking to move it.
    h.receive(Opcode::SMSG_BINDPOINTUPDATE, Body().f32(-9459.0f).f32(42.0f).f32(57.0f).u32(0).u32(12));
    h.receive(Opcode::SMSG_BINDER_CONFIRM, Body().u64(kInnkeeper));
    // Weapons and armour the character can use.
    h.receive(Opcode::SMSG_SET_PROFICIENCY, Body().u8(2).u32(0x5));
    h.receive(Opcode::SMSG_SET_PROFICIENCY, Body().u8(4).u32(0x3));
    // Holding its breath under water.
    h.receive(Opcode::SMSG_START_MIRROR_TIMER, Body().u32(1).u32(90000).u32(180000).u32(static_cast<uint32_t>(-1)).u8(0).u32(0));
    // A blood rune turned to death.
    h.receive(Opcode::SMSG_CONVERT_RUNE, Body().u8(0).u8(3));
    // In the barber's chair, and in a vehicle.
    h.receive(Opcode::SMSG_ENABLE_BARBER_SHOP);
    h.receive(Opcode::SMSG_PLAYER_VEHICLE_DATA, Body().packedGuid(alice.guid).u32(244));
    // Three combo points on a boar, a faction forced hostile, and a talent
    // reset offered.
    h.receive(Opcode::SMSG_UPDATE_COMBO_POINTS, Body().packedGuid(kBoar).u8(3));
    h.receive(Opcode::SMSG_SET_FORCED_REACTIONS, Body().u32(1).u32(72).u32(0));
    h.receive(Opcode::MSG_TALENT_WIPE_CONFIRM, Body().u64(kTrainer).u32(10000));
    h.pump();
    // A heading the player closed in the skills panel.
    h.game().setSkillCategoryCollapsed(9, true);

    uint32_t homeMap = 0;
    glm::vec3 homePos{};
    REQUIRE(h.game().getHomeBind(homeMap, homePos));
    REQUIRE(h.game().getBinderGuid() == kInnkeeper);
    REQUIRE(h.game().getWeaponProficiency() == 0x5);
    REQUIRE(h.game().getMirrorTimer(1).active);
    REQUIRE(h.game().getPlayerRunes()[0].type == wowee::game::GameHandler::RuneType::Death);
    REQUIRE(h.game().isBarberShopOpen());
    REQUIRE(h.game().isInVehicle());
    REQUIRE(h.game().combat().comboPoints() == 3);
    REQUIRE_FALSE(h.game().combat().getForcedReactions().empty());
    REQUIRE(h.game().spells().showTalentWipeConfirmDialog());
    REQUIRE(h.game().isSkillCategoryCollapsed(9));

    h.relogAs(bob);

    CHECK_FALSE(h.game().getHomeBind(homeMap, homePos));
    CHECK_FALSE(h.game().homeBind().has_value());
    CHECK(h.game().getHomeBindZoneId() == 0);
    CHECK(h.game().getBinderGuid() == 0);
    CHECK(h.game().getWeaponProficiency() == 0);
    CHECK(h.game().getArmorProficiency() == 0);
    CHECK_FALSE(h.game().getMirrorTimer(1).active);
    CHECK(h.game().getPlayerRunes()[0].type == wowee::game::GameHandler::RuneType::Blood);
    CHECK_FALSE(h.game().isBarberShopOpen());
    CHECK_FALSE(h.game().isInVehicle());
    CHECK(h.game().combat().comboPoints() == 0);
    CHECK(h.game().combat().getForcedReactions().empty());
    CHECK_FALSE(h.game().spells().showTalentWipeConfirmDialog());
    CHECK_FALSE(h.game().isSkillCategoryCollapsed(9));
}

TEST_CASE("suspending client movement sends stop packets before controls are frozen", "[session][harness][loading]") {
    GameHarness h;
    h.enterWorld();
    h.game().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    h.game().sendMovement(Opcode::MSG_MOVE_START_STRAFE_LEFT);
    h.game().sendMovement(Opcode::MSG_MOVE_START_TURN_RIGHT);
    h.game().sendMovement(Opcode::MSG_MOVE_START_PITCH_UP);
    h.game().sendMovement(Opcode::MSG_MOVE_START_ASCEND);
    h.server().clearSent();
    h.game().movement().stopInputMovement();
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
    h.game().movement().stopInputMovement();
    CHECK(h.sentOpcodes().empty());
}

TEST_CASE("network dispatch receives logout state and only Game advances its countdown",
          "[session][harness][frame]") {
    GameHarness h;
    h.enterWorld();
    h.game().social().requestLogout();
    h.receive(Opcode::SMSG_LOGOUT_RESPONSE, Body().u32(0).u8(0));
    h.game().updateNetwork(0.0f);
    REQUIRE(h.game().social().isLoggingOut());
    const float countdown = h.game().social().getLogoutCountdown();
    REQUIRE(countdown > 0);
    h.game().updateNetwork(3.0f);
    CHECK(h.game().social().getLogoutCountdown() == countdown);
    h.game().updateGame(3.0f);
    CHECK(h.game().social().getLogoutCountdown() == Catch::Approx(countdown - 3.0f));
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

    h.game().social().requestLogout(exiting);
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
    auto despawnSub = h.events().subscribe<ev::CreatureDespawned>(
        [despawns, creatureGuid = creature->getGuid()](const ev::CreatureDespawned& e) {
            CHECK(e.guid == creatureGuid);
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
    h.game().movement().applyPendingWorldTransfer();

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
    h.game().social().requestLogout(true); // no connection: local exit, no server completion
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
    CHECK_FALSE(h.game().movement().hasPendingWorldTransfer());
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
    auto creatureSub = h.events().subscribe<ev::CreatureSpawned>([&](const ev::CreatureSpawned& e) {
        creatures.push_back(e.guid);
        if (e.guid == creature->getGuid()) {
            CHECK(e.displayId == 4321);
            CHECK(e.x == 10.0f);
            CHECK(e.y == 20.0f);
            CHECK(e.z == 30.0f);
            CHECK(e.orientation == 1.5f);
        }
    });
    auto deathSub = h.events().subscribe<ev::UnitDied>([&](const ev::UnitDied& e) { deaths.push_back(e.guid); });
    auto mountSub = h.events().subscribe<ev::OtherPlayerMounted>(
        [&](const ev::OtherPlayerMounted& e) { mounts.emplace_back(e.guid, e.mountDisplayId); });
    auto stateSub = h.events().subscribe<ev::GameObjectStateChanged>(
        [&](const ev::GameObjectStateChanged& e) { doorStates.emplace_back(e.guid, e.goState); });
    auto objectSub = h.events().subscribe<ev::GameObjectSpawned>([&](const ev::GameObjectSpawned& e) {
        objects.push_back(e.guid);
        CHECK(e.entry == 77);
        CHECK(e.displayId == 411);
    });
    h.server().clearSent();

    h.game().world().announceObjectsInView();

    std::sort(creatures.begin(), creatures.end());
    CHECK(creatures == std::vector<uint64_t>{creature->getGuid(), corpse->getGuid()});
    CHECK(deaths == std::vector<uint64_t>{corpse->getGuid()});
    CHECK(mounts == std::vector<std::pair<uint64_t, uint32_t>>{{rider->getGuid(), 14337}});
    CHECK(doorStates == std::vector<std::pair<uint64_t, uint8_t>>{{door->getGuid(), 0}});
    CHECK(objects == std::vector<uint64_t>{door->getGuid()});
    CHECK(h.sentOpcodes().empty());
}

TEST_CASE("an object the server destroys leaves view as what it is", "[session][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    auto creature = std::make_shared<Unit>(0x2001);
    auto player = std::make_shared<Player>(0x2002);
    auto chest = std::make_shared<GameObject>(0x2003);
    for (const auto& entity : std::initializer_list<std::shared_ptr<Entity>>{creature, player, chest})
        h.game().getEntityManager().addEntity(entity->getGuid(), entity);

    std::vector<std::pair<char, uint64_t>> left;
    auto creatureSub = h.events().subscribe<ev::CreatureDespawned>(
        [&](const ev::CreatureDespawned& e) { left.emplace_back('c', e.guid); });
    auto playerSub = h.events().subscribe<ev::PlayerDespawned>(
        [&](const ev::PlayerDespawned& e) { left.emplace_back('p', e.guid); });
    auto objectSub = h.events().subscribe<ev::GameObjectDespawned>(
        [&](const ev::GameObjectDespawned& e) { left.emplace_back('g', e.guid); });

    // SMSG_DESTROY_OBJECT: the guid, and whether it died (WotLK).
    h.receive(Opcode::SMSG_DESTROY_OBJECT, Body().u64(0x2001).u8(0));
    h.receive(Opcode::SMSG_DESTROY_OBJECT, Body().u64(0x2002).u8(0));
    h.receive(Opcode::SMSG_DESTROY_OBJECT, Body().u64(0x2003).u8(0));
    h.pump();

    CHECK(left == std::vector<std::pair<char, uint64_t>>{{'c', 0x2001}, {'p', 0x2002}, {'g', 0x2003}});
    CHECK_FALSE(h.game().getEntityManager().hasEntity(0x2002));
}

TEST_CASE("a game object's custom animation is played; the player's own bobber plays its bite",
          "[session][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    auto chest = std::make_shared<GameObject>(0x3001);
    auto bobber = std::make_shared<GameObject>(0x3002);
    // OBJECT_FIELD_CREATED_BY, as two words: the player cast it.
    bobber->setField(6, static_cast<uint32_t>(h.player().guid & 0xFFFFFFFFu));
    bobber->setField(7, static_cast<uint32_t>(h.player().guid >> 32));
    h.game().getEntityManager().addEntity(chest->getGuid(), chest);
    h.game().getEntityManager().addEntity(bobber->getGuid(), bobber);

    std::vector<std::pair<uint64_t, uint32_t>> played;
    auto sub = h.events().subscribe<ev::GameObjectCustomAnimPlayed>(
        [&](const ev::GameObjectCustomAnimPlayed& e) { played.emplace_back(e.guid, e.animId); });

    // SMSG_GAMEOBJECT_CUSTOM_ANIM: the object, then an animation id - for a
    // bobber, AzerothCore's animation progress (100), not a sequence.
    h.receive(Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM, Body().u64(0x3001).u32(2));
    h.receive(Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM, Body().u64(0x3002).u32(100));
    h.pump();

    // G_FishingBobber.m2's bite and splash is sequence 153.
    CHECK(played == std::vector<std::pair<uint64_t, uint32_t>>{{0x3001, 2}, {0x3002, 153}});
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
    auto playerSub = h.events().subscribe<ev::PlayerSpawned>([&](const ev::PlayerSpawned&) { ++spawns; });
    auto creatureSub = h.events().subscribe<ev::CreatureSpawned>([&](const ev::CreatureSpawned&) { ++spawns; });
    h.game().world().announceObjectsInView();
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
        REQUIRE(h.game().movement().hasPendingWorldTransfer());
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

    h.game().spells().castSpell(133, 0);
    h.game().chat().sendChatMessage(wowee::game::ChatType::SAY, "anyone there?", "");
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
    h.game().spells().castSpell(133, 0);
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
    REQUIRE(h.game().spells().getSpellCooldown(kFireball) > 0.0f);
    REQUIRE_FALSE(h.game().spells().getPlayerAuras().empty());
    REQUIRE(h.group().isInGroup());
    REQUIRE_FALSE(h.game().inventory().items().getBackpackSlot(0).empty());

    h.server().disconnect();
    h.pump();
    reconnect(h);

    // The new session is the server's to fill; nothing of the last is left.
    CHECK(h.game().spells().getSpellCooldown(kFireball) == 0.0f);
    CHECK(h.game().spells().getPlayerAuras().empty());
    CHECK_FALSE(h.group().isInGroup());
    CHECK(h.game().inventory().items().getBackpackSlot(0).empty());
    CHECK_FALSE(h.game().getEntityManager().hasEntity(kItem));
}

TEST_CASE("exploration experience is announced; an area found for nothing is not", "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::AreaDiscovered> found;
    auto sub = h.events().subscribe<ev::AreaDiscovered>([&](const ev::AreaDiscovered& e) { found.push_back(e); });

    // SMSG_EXPLORATION_EXPERIENCE: the area, then the experience it gave.
    h.receive(Opcode::SMSG_EXPLORATION_EXPERIENCE, Body().u32(12).u32(170).bytes());
    h.receive(Opcode::SMSG_EXPLORATION_EXPERIENCE, Body().u32(40).u32(0).bytes());
    h.pump();

    REQUIRE(found.size() == 1);
    CHECK(found[0].xpGained == 170);
}

TEST_CASE("sounds the server plays are passed on: music, flat, and at an object", "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<uint32_t> music, flat;
    std::vector<std::pair<uint32_t, uint64_t>> placed;
    auto musicSub = h.events().subscribe<ev::MusicRequested>(
        [&](const ev::MusicRequested& e) { music.push_back(e.soundId); });
    auto flatSub = h.events().subscribe<ev::SoundRequested>(
        [&](const ev::SoundRequested& e) { flat.push_back(e.soundId); });
    auto placedSub = h.events().subscribe<ev::PositionalSoundRequested>(
        [&](const ev::PositionalSoundRequested& e) { placed.emplace_back(e.soundId, e.sourceGuid); });

    h.receive(Opcode::SMSG_PLAY_MUSIC, Body().u32(2532));
    h.receive(Opcode::SMSG_PLAY_SOUND, Body().u32(8960));
    // SMSG_PLAY_OBJECT_SOUND: the sound, then the object it comes from; a
    // server that leaves the object out has it played flat.
    h.receive(Opcode::SMSG_PLAY_OBJECT_SOUND, Body().u32(6447).u64(0xF130000000004242ull));
    h.receive(Opcode::SMSG_PLAY_OBJECT_SOUND, Body().u32(6448));
    h.pump();

    CHECK(music == std::vector<uint32_t>{2532});
    CHECK(flat == std::vector<uint32_t>{8960, 6448});
    CHECK(placed == std::vector<std::pair<uint32_t, uint64_t>>{{6447, 0xF130000000004242ull}});
}

TEST_CASE("the server's answers to creating and deleting a character are passed on in words",
          "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<std::pair<bool, std::string>> created, deleted;
    auto createSub = h.events().subscribe<ev::CharacterCreateAnswered>(
        [&](const ev::CharacterCreateAnswered& e) { created.emplace_back(e.success, e.message); });
    auto deleteSub = h.events().subscribe<ev::CharacterDeleteAnswered>(
        [&](const ev::CharacterDeleteAnswered& e) { deleted.emplace_back(e.success, e.message); });

    // SMSG_CHAR_CREATE: 0x32 the name is taken, 0x2F created.
    h.receive(Opcode::SMSG_CHAR_CREATE, Body().u8(0x32));
    h.receive(Opcode::SMSG_CHAR_CREATE, Body().u8(0x2F));
    // SMSG_CHAR_DELETE: 0x31 a guild leader, 0x47 deleted.
    h.receive(Opcode::SMSG_CHAR_DELETE, Body().u8(0x31));
    h.receive(Opcode::SMSG_CHAR_DELETE, Body().u8(0x47));
    h.pump();

    CHECK(created == std::vector<std::pair<bool, std::string>>{
                         {false, "Name already in use"}, {true, "Character created!"}});
    CHECK(deleted == std::vector<std::pair<bool, std::string>>{
                         {false, "Delete failed: character is a guild leader. Transfer leadership first."},
                         {true, "Character deleted."}});
}

TEST_CASE("a knock-back is passed on for the player and not for anyone else", "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::PlayerKnockedBack> knocked;
    auto sub = h.events().subscribe<ev::PlayerKnockedBack>(
        [&](const ev::PlayerKnockedBack& e) { knocked.push_back(e); });

    // SMSG_MOVE_KNOCK_BACK: the unit, a counter, the direction (cos, sin),
    // then the horizontal and vertical speeds.
    h.receive(Opcode::SMSG_MOVE_KNOCK_BACK,
              Body().packedGuid(0xF130000000000099ull).u32(1).f32(1.0f).f32(0.0f).f32(5.0f).f32(-7.0f));
    h.receive(Opcode::SMSG_MOVE_KNOCK_BACK,
              Body().packedGuid(h.player().guid).u32(2).f32(0.6f).f32(0.8f).f32(10.0f).f32(-9.0f));
    h.pump();

    REQUIRE(knocked.size() == 1);
    CHECK(knocked[0].vcos == 0.6f);
    CHECK(knocked[0].vsin == 0.8f);
    CHECK(knocked[0].hspeed == 10.0f);
    CHECK(knocked[0].vspeed == -9.0f);
}

TEST_CASE("a camera shake is light for the first fifty shakes and heavy after", "[session][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::CameraShakeRequested> shakes;
    auto sub = h.events().subscribe<ev::CameraShakeRequested>(
        [&](const ev::CameraShakeRequested& e) { shakes.push_back(e); });

    // SMSG_CAMERA_SHAKE: the CameraShakes.dbc id, then the shake type.
    h.receive(Opcode::SMSG_CAMERA_SHAKE, Body().u32(10).u32(0));
    h.receive(Opcode::SMSG_CAMERA_SHAKE, Body().u32(60).u32(0));
    h.pump();

    REQUIRE(shakes.size() == 2);
    CHECK(shakes[0].magnitude == 0.04f);
    CHECK(shakes[1].magnitude == 0.08f);
    CHECK(shakes[1].frequency == 18.0f);
    CHECK(shakes[1].duration == 0.5f);
}

TEST_CASE("the map the player is on is named, and is an instance, as Map.dbc says",
          "[session][harness][tables]") {
    GameHarness h;
    // ID, internal name, instance type (1 = party), flags, display name.
    h.dbc("Map.dbc", 5, {{{0, 0}, {1, "Azeroth"}, {2, 0}, {4, "Eastern Kingdoms"}},
                         {{0, 33}, {1, "Shadowfang"}, {2, 1}, {4, "Shadowfang Keep"}}});
    wowee::test::PlayerSpec inKeep;
    inKeep.mapId = 33;
    h.enterWorld(inKeep);

    CHECK(h.game().getMapName(0) == "Eastern Kingdoms");
    CHECK(h.game().getMapName(33) == "Shadowfang Keep");
    CHECK(h.game().isInInstance());
}

TEST_CASE("the open world is not an instance, by its Map.dbc row", "[session][harness][tables]") {
    GameHarness h;
    h.dbc("Map.dbc", 5, {{{0, 0}, {1, "Azeroth"}, {2, 0}, {4, "Eastern Kingdoms"}},
                         {{0, 33}, {1, "Shadowfang"}, {2, 1}, {4, "Shadowfang Keep"}}});
    h.enterWorld();
    CHECK_FALSE(h.game().isInInstance());
}

TEST_CASE("an area is named by AreaTable.dbc, and by WorldMapArea.dbc where it has no row there",
          "[session][harness][tables]") {
    GameHarness h;
    h.dbc("AreaTable.dbc", 36, {{{0, 12}, {11, "Elwynn Forest"}}});
    // AreaID 2 and AreaName 3; a row for an area AreaTable also names does not
    // override it.
    h.dbc("WorldMapArea.dbc", 11, {{{0, 1}, {2, 12}, {3, "Elwynn"}},
                                   {{0, 2}, {2, 4000}, {3, "Somewhere Else"}}});
    h.enterWorld();

    CHECK(h.game().getAreaName(12) == "Elwynn Forest");
    CHECK(h.game().getAreaName(4000) == "Somewhere Else");
    CHECK(h.game().getAreaName(99).empty());
}

TEST_CASE("a continent's map rectangle is its own WorldMapArea.dbc row, not a zone's",
          "[session][harness][tables]") {
    GameHarness h;
    // MapID 1, AreaID 2, then left, right, top, bottom at 4-7.
    h.dbc("WorldMapArea.dbc", 11, {
        {{0, 1}, {1, 0}, {2, 12}, {4, 100.0f}, {5, -100.0f}, {6, 50.0f}, {7, -50.0f}},
        {{0, 2}, {1, 0}, {2, 0}, {4, 4000.0f}, {5, -4000.0f}, {6, 8000.0f}, {7, -6000.0f}},
    });
    h.enterWorld();

    const auto& bounds = h.game().getContinentBounds(0);
    REQUIRE(bounds.valid);
    CHECK(bounds.left == 4000.0f);
    CHECK(bounds.right == -4000.0f);
    CHECK(bounds.top == 8000.0f);
    CHECK(bounds.bottom == -6000.0f);
    CHECK_FALSE(h.game().getContinentBounds(1).valid);
}

TEST_CASE("another player is drawn as their fields say, a field left out being zero",
          "[session][harness]") {
    using wowee::game::ObjectType;
    using wowee::game::UF;
    auto uf = [](UF f) { return static_cast<uint16_t>(wowee::game::fieldIndex(f)); };
    constexpr uint64_t kOther = 0x0000000000000777ull;
    GameHarness h;
    h.enterWorld();
    std::vector<ev::PlayerSpawned> spawned;
    auto sub = h.events().subscribe<ev::PlayerSpawned>(
        [&](const ev::PlayerSpawned& e) { spawned.push_back(e); });

    // A Tauren (race 6) woman, a druid (class 11). Her skin, face, hair style,
    // hair colour and markings are all the first: PLAYER_BYTES and
    // PLAYER_BYTES_2 are zero, and a create block leaves a zero field out.
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kOther, ObjectType::PLAYER,
                                            {{uf(UF::UNIT_FIELD_BYTES_0), 6u | (11u << 8) | (1u << 16)},
                                             {uf(UF::UNIT_FIELD_HEALTH), 100},
                                             {uf(UF::UNIT_FIELD_DISPLAYID), 2589}}));
    h.pump();

    REQUIRE(spawned.size() == 1);
    CHECK(spawned[0].raceId == 6);
    CHECK(spawned[0].genderId == 1);
    CHECK(spawned[0].appearanceBytes == 0u);
    CHECK(spawned[0].facialFeatures == 0);
}

TEST_CASE("a player whose fields carry no race is not drawn as one", "[session][harness]") {
    using wowee::game::ObjectType;
    using wowee::game::UF;
    auto uf = [](UF f) { return static_cast<uint16_t>(wowee::game::fieldIndex(f)); };
    GameHarness h;
    h.enterWorld();
    std::vector<ev::PlayerSpawned> spawned;
    auto sub = h.events().subscribe<ev::PlayerSpawned>(
        [&](const ev::PlayerSpawned& e) { spawned.push_back(e); });

    // No UNIT_FIELD_BYTES_0, but plenty of small fields a guess could take
    // for one.
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(0x778, ObjectType::PLAYER,
                                            {{uf(UF::UNIT_FIELD_HEALTH), 100},
                                             {uf(UF::UNIT_FIELD_LEVEL), 0x00010106},
                                             {uf(UF::UNIT_FIELD_DISPLAYID), 2589}}));
    h.pump();
    CHECK(spawned.empty());
}
