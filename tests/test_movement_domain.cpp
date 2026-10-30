// Movement, end to end: what the server says of where things are and go,
// and what the rest of the client is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/entity.hpp"
#include "game/entity_controller.hpp"
#include "game/game_events.hpp"

#include <glm/vec3.hpp>

#include <memory>
#include <vector>

using wowee::test::GameHarness;
namespace ev = wowee::game::events;

TEST_CASE("following stops on the next frame once the followed player leaves view", "[movement][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    const auto& me = h.game().getMovementInfo();
    auto friendPlayer = std::make_shared<Player>(0xB001);
    friendPlayer->setName("Anna");
    friendPlayer->setPosition(me.x + 3.0f, me.y, me.z, 0.0f);
    h.game().getEntityManager().addEntity(friendPlayer->getGuid(), friendPlayer);
    std::vector<bool> following;
    auto sub = h.events().subscribe<ev::AutoFollowChanged>(
        [&](const ev::AutoFollowChanged& e) { following.push_back(e.renderPos != nullptr); });

    h.game().combat().setTarget(0xB001);
    h.game().movement().followTarget();
    h.pump();
    REQUIRE(following == std::vector<bool>{true});
    REQUIRE(h.addonEvents("AUTOFOLLOW_BEGIN").size() == 1);
    CHECK(h.addonEvents("AUTOFOLLOW_BEGIN")[0].args.at(0) == "Anna");

    h.game().getEntityManager().removeEntity(0xB001);
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
    h.game().entityController().applyRecordedObjects(batch);
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
    h.game().entityController().applyRecordedObjects(batch);
}

void enterWorldAsEntity(GameHarness& h) {
    h.enterWorld();
    auto self = std::make_shared<wowee::game::Player>(h.player().guid);
    h.game().getEntityManager().addEntity(self->getGuid(), self);
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
    auto creature = std::make_shared<wowee::game::Unit>(0xC001);
    h.game().getEntityManager().addEntity(creature->getGuid(), creature);
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
    auto creature = std::make_shared<wowee::game::Unit>(0xC002);
    h.game().getEntityManager().addEntity(creature->getGuid(), creature);
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
