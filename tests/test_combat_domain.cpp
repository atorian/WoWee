// Combat, end to end: what the server says of a fight and what the rest of
// the client is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/entity.hpp"
#include "game/entity_controller.hpp"
#include "game/game_events.hpp"
#include "game/update_field_table.hpp"

#include <glm/vec3.hpp>

#include <memory>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

TEST_CASE("honor credited for a kill is announced with the victim and their rank", "[combat][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<ev::HonorGained> gained;
    auto sub = h.events().subscribe<ev::HonorGained>([&](const ev::HonorGained& e) { gained.push_back(e); });

    // SMSG_PVP_CREDIT: the honor, the victim, the victim's PvP rank.
    h.receive(Opcode::SMSG_PVP_CREDIT, Body().u32(21).u64(0x0000000000004242ull).u32(7).bytes());
    h.pump();

    REQUIRE(gained.size() == 1);
    CHECK(gained[0].honor == 21);
    CHECK(gained[0].victimGuid == 0x4242ull);
    CHECK(gained[0].victimRank == 7);
}

TEST_CASE("an NPC turning hostile is announced where it stands; another reaction is not", "[combat][harness]") {
    GameHarness h;
    h.enterWorld();
    auto wolf = std::make_shared<wowee::game::Unit>(0x5001);
    wolf->setPosition(1.0f, 2.0f, 3.0f, 0.0f);
    h.game().getEntityManager().addEntity(wolf->getGuid(), wolf);
    std::vector<ev::NpcAggroed> aggro;
    auto sub = h.events().subscribe<ev::NpcAggroed>([&](const ev::NpcAggroed& e) { aggro.push_back(e); });

    // SMSG_AI_REACTION: the unit, then the reaction - 2 is hostile.
    h.receive(Opcode::SMSG_AI_REACTION, Body().u64(0x5001).u32(0));
    h.receive(Opcode::SMSG_AI_REACTION, Body().u64(0x5001).u32(2));
    h.pump();

    REQUIRE(aggro.size() == 1);
    CHECK(aggro[0].guid == 0x5001);
    CHECK(aggro[0].x == 1.0f);
    CHECK(aggro[0].y == 2.0f);
    CHECK(aggro[0].z == 3.0f);
}

TEST_CASE("a swing at a target just out of reach says so once", "[combat][harness]") {
    GameHarness h;
    h.enterWorld();
    const auto& me = h.game().getMovementInfo();
    // Inside the 8 yards auto-attack starts at, outside the 5.75 a sword
    // reaches.
    auto boar = std::make_shared<wowee::game::Unit>(0x6001);
    boar->setPosition(me.x + 6.5f, me.y, me.z, 0.0f);
    h.game().getEntityManager().addEntity(boar->getGuid(), boar);
    std::vector<std::string> raised;
    auto sub = h.events().subscribe<ev::UiErrorRaised>([&](const ev::UiErrorRaised& e) { raised.push_back(e.message); });

    h.game().combat().startAutoAttack(0x6001);
    h.pump();

    std::vector<std::string> shown;
    for (const auto& e : h.addonEvents("UI_ERROR_MESSAGE")) shown.push_back(e.args.at(0));
    CHECK(shown == std::vector<std::string>{"Target is too far away."});
    CHECK(raised == std::vector<std::string>{"Target is too far away."});
}

TEST_CASE("combo points are the ones the server last stated", "[combat][harness]") {
    GameHarness h;
    h.enterWorld();

    // SMSG_UPDATE_COMBO_POINTS: the target, then the points on it.
    h.receive(Opcode::SMSG_UPDATE_COMBO_POINTS, Body().packedGuid(0x7001).u8(3));
    h.pump();
    CHECK(h.game().combat().comboPoints() == 3);
    CHECK(h.addonEvents("PLAYER_COMBO_POINTS").size() == 1);

    h.receive(Opcode::SMSG_UPDATE_COMBO_POINTS, Body().packedGuid(0x7001).u8(0));
    h.pump();
    CHECK(h.game().combat().comboPoints() == 0);
}

namespace {
std::shared_ptr<wowee::game::Unit> hostileAt(GameHarness& h, uint64_t guid, float yardsAway) {
    const auto& me = h.game().getMovementInfo();
    auto unit = std::make_shared<wowee::game::Unit>(guid);
    unit->setPosition(me.x + yardsAway, me.y, me.z, 0.0f);
    unit->setMaxHealth(100);
    unit->setHealth(100);
    unit->setHostile(true);
    return unit;
}
}  // namespace

TEST_CASE("Tab steps through enemies nearest first, and takes in one that has just come into view",
          "[combat][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    const auto& me = h.game().getMovementInfo();
    for (const auto& unit : {hostileAt(h, 0x8001, 5.0f), hostileAt(h, 0x8002, 10.0f)})
        h.game().getEntityManager().addEntity(unit->getGuid(), unit);

    h.game().combat().tabTarget(me.x, me.y, me.z);
    CHECK(h.game().getTargetGuid() == 0x8001);
    h.game().combat().tabTarget(me.x, me.y, me.z);
    CHECK(h.game().getTargetGuid() == 0x8002);

    // A third, nearer than both, arrives in an update from the server.
    UpdateBlock block{};
    block.guid = 0x8003;
    block.objectType = ObjectType::UNIT;
    block.updateType = UpdateType::CREATE_OBJECT;
    block.fields[fieldIndex(UF::UNIT_FIELD_DISPLAYID)] = 1234;
    block.fields[fieldIndex(UF::UNIT_FIELD_HEALTH)] = 100;
    block.fields[fieldIndex(UF::UNIT_FIELD_MAXHEALTH)] = 100;
    UpdateObjectData batch{};
    batch.blocks.push_back(block);
    h.game().entityController().applyRecordedObjects(batch);
    auto arrived = std::static_pointer_cast<Unit>(h.game().getEntityManager().getEntity(0x8003));
    REQUIRE(arrived);
    arrived->setPosition(me.x + 3.0f, me.y, me.z, 0.0f);
    arrived->setHostile(true);

    // From the farthest the list now goes round to the newcomer, nearest of all.
    h.game().combat().tabTarget(me.x, me.y, me.z);
    CHECK(h.game().getTargetGuid() == 0x8003);
}

// ---- The player's death ----

TEST_CASE("a forced death leaves the player dead, the corpse where they stood", "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();
    const auto& me = h.game().getMovementInfo();

    h.receive(Opcode::SMSG_FORCED_DEATH_UPDATE, Body());
    h.pump();

    CHECK(h.game().isPlayerDead());
    CHECK_FALSE(h.game().isPlayerGhost());
    CHECK(h.addonEvents("PLAYER_DEAD").size() == 1);
    float corpseX = 0, corpseY = 0;
    REQUIRE(h.game().getCorpseCanonicalPos(corpseX, corpseY));
    CHECK(corpseX == me.x);
    CHECK(corpseY == me.y);
}

TEST_CASE("a player's resurrection is offered by name and accepted with a response", "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();

    // SMSG_RESURRECT_REQUEST: the caster, the name's length and the name, a
    // second terminator, then sickness (0 from a player); no trailing word, so
    // the reclaim delay applies.
    h.receive(Opcode::SMSG_RESURRECT_REQUEST,
              Body().u64(0x0000000000000777ull).u32(5).cstr("Anna").u8(0).u8(0));
    h.pump();

    const auto offered = h.addonEvents("RESURRECT_REQUEST");
    REQUIRE(offered.size() == 1);
    CHECK(offered[0].args.at(0) == "Anna");
    CHECK_FALSE(h.game().resurrectHasSickness());
    CHECK(h.game().resurrectHasTimer());

    h.game().combat().acceptResurrect();
    auto* sent = h.lastSent(Opcode::CMSG_RESURRECT_RESPONSE);
    REQUIRE(sent);
    CHECK(sent->readUInt64() == 0x777ull);
    CHECK(sent->readUInt8() == 1);
    CHECK(h.countSent(Opcode::CMSG_SPIRIT_HEALER_ACTIVATE) == 0);
}

TEST_CASE("a spirit healer's offer asks to confirm the loss, and accepting it activates the healer",
          "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_SPIRIT_HEALER_CONFIRM, Body().u64(0xF130000000001234ull));
    h.pump();
    CHECK(h.addonEvents("CONFIRM_XP_LOSS").size() == 1);

    h.game().combat().acceptResurrect();
    auto* sent = h.lastSent(Opcode::CMSG_SPIRIT_HEALER_ACTIVATE);
    REQUIRE(sent);
    CHECK(sent->readUInt64() == 0xF130000000001234ull);
    CHECK(h.countSent(Opcode::CMSG_RESURRECT_RESPONSE) == 0);
}

TEST_CASE("the graveyard the server names is where a release goes, until it withdraws it",
          "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();

    // SMSG_DEATH_RELEASE_LOC: the map, then server x, y, z.
    h.receive(Opcode::SMSG_DEATH_RELEASE_LOC, Body().u32(1).f32(100.0f).f32(200.0f).f32(30.0f));
    h.pump();
    uint32_t map = 99;
    glm::vec3 where{};
    REQUIRE(h.game().getDeathReleaseLocation(map, where));
    CHECK(map == 1);
    // Canonical is north, west: the server's y, x.
    CHECK(where.x == 200.0f);
    CHECK(where.y == 100.0f);
    CHECK(where.z == 30.0f);

    h.receive(Opcode::SMSG_DEATH_RELEASE_LOC, Body().u32(0xFFFFFFFFu).f32(0).f32(0).f32(0));
    h.pump();
    CHECK_FALSE(h.game().getDeathReleaseLocation(map, where));
}

TEST_CASE("the corpse query says where to walk, on the map those coordinates are on",
          "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();
    const uint32_t here = h.game().getCurrentMapId();

    // MSG_CORPSE_QUERY: found, the map to walk on, server x, y, z, the map the
    // corpse lies on (a dungeon behind that entrance), and a word nobody reads.
    h.receive(Opcode::MSG_CORPSE_QUERY,
              Body().u8(1).u32(here).f32(10.0f).f32(20.0f).f32(5.0f).u32(here + 1000).u32(0));
    h.pump();

    float corpseX = 0, corpseY = 0;
    REQUIRE(h.game().getCorpseCanonicalPos(corpseX, corpseY));
    CHECK(corpseX == 20.0f);
    CHECK(corpseY == 10.0f);
}

TEST_CASE("self-resurrection is offered to the player and taken with CMSG_SELF_RES", "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();
    REQUIRE_FALSE(h.game().canSelfRes());

    h.receive(Opcode::SMSG_PRE_RESURRECT, Body().packedGuid(0x0000000000000555ull));
    h.pump();
    CHECK_FALSE(h.game().canSelfRes());   // someone else's

    h.receive(Opcode::SMSG_PRE_RESURRECT, Body().packedGuid(h.player().guid));
    h.pump();
    CHECK(h.game().canSelfRes());

    h.game().combat().useSelfRes();
    CHECK(h.countSent(Opcode::CMSG_SELF_RES) == 1);
}

TEST_CASE("an area spirit healer's countdown is shown and its queue is joined", "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();

    // SMSG_AREA_SPIRIT_HEALER_TIME: the healer, then milliseconds to go.
    h.receive(Opcode::SMSG_AREA_SPIRIT_HEALER_TIME, Body().u64(0xF130000000004321ull).u32(25000));
    h.pump();
    CHECK(h.game().getAreaSpiritHealerTime() == 25.0f);

    h.game().queueAreaSpiritHeal();
    auto* sent = h.lastSent(Opcode::CMSG_AREA_SPIRIT_HEALER_QUEUE);
    REQUIRE(sent);
    CHECK(sent->readUInt64() == 0xF130000000004321ull);
}

TEST_CASE("the ghost flag makes the player a ghost, and clearing it brings them back without a corpse",
          "[combat][death][harness]") {
    using namespace wowee::game;
    GameHarness h;
    h.enterWorld();
    auto self = std::make_shared<Player>(h.player().guid);
    h.game().getEntityManager().addEntity(self->getGuid(), self);
    h.receive(Opcode::SMSG_FORCED_DEATH_UPDATE, Body());
    h.pump();

    auto flags = [&](uint32_t value) {
        UpdateBlock block{};
        block.guid = h.player().guid;
        block.objectType = ObjectType::PLAYER;
        block.updateType = UpdateType::VALUES;
        block.fields[fieldIndex(UF::PLAYER_FLAGS)] = value;
        UpdateObjectData batch{};
        batch.blocks.push_back(block);
        h.game().entityController().applyRecordedObjects(batch);
    };
    flags(0x10);   // PLAYER_FLAGS_GHOST
    CHECK(h.game().isPlayerGhost());

    flags(0);
    h.pump();
    CHECK_FALSE(h.game().isPlayerGhost());
    CHECK_FALSE(h.game().isPlayerDead());
    float corpseX = 0, corpseY = 0;
    CHECK_FALSE(h.game().getCorpseCanonicalPos(corpseX, corpseY));
    CHECK(h.addonEvents("PLAYER_ALIVE").size() == 1);
}

TEST_CASE("a corpse reclaim delay is counted down from the server's milliseconds", "[combat][death][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_CORPSE_RECLAIM_DELAY, Body().u32(30000));
    h.pump();
    const float left = h.game().combat().getCorpseReclaimDelaySec();
    CHECK(left > 29.0f);
    CHECK(left <= 30.0f);
}
