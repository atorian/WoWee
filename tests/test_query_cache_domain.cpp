// The query caches, end to end: what the realm says a guid or an entry is -
// players' names, creature and game object templates - asked for once, kept,
// and handed to the objects in view and the interface when it arrives.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/entity.hpp"
#include "game/entity_controller.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::UF;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {
constexpr uint64_t kThrall = 0x0000000000000042ull;
constexpr uint64_t kGnoll = 0xF130000075000001ull;
constexpr uint64_t kSign = 0xF110000400000001ull;

uint16_t uf(UF f) { return static_cast<uint16_t>(wowee::game::fieldIndex(f)); }

size_t sent(const GameHarness& h, Opcode op) {
    const auto ops = h.sentOpcodes();
    return static_cast<size_t>(std::count(ops.begin(), ops.end(), op));
}

void playerComesIntoView(GameHarness& h, uint64_t guid) {
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(guid, ObjectType::PLAYER,
                                            {{uf(UF::UNIT_FIELD_HEALTH), 100},
                                             {uf(UF::UNIT_FIELD_DISPLAYID), 2589}}));
    h.pump();
}

std::string nameInView(GameHarness& h, uint64_t guid) {
    auto entity = h.game().getEntityManager().getEntity(guid);
    REQUIRE(entity);
    REQUIRE(entity->isUnit());
    return std::static_pointer_cast<wowee::game::Unit>(entity)->getName();
}

/// SMSG_CREATURE_QUERY_RESPONSE: the names, then type flags, type, family
/// and rank.
std::vector<uint8_t> creatureResponse(uint32_t entry, const std::string& name, uint32_t rank) {
    return Body().u32(entry).cstr(name).cstr("").cstr("").cstr("").cstr("").cstr("")
        .u32(0).u32(7).u32(0).u32(rank).bytes();
}

/// SMSG_GAMEOBJECT_QUERY_RESPONSE as 3.3.5a writes it, a plain sign (type 5).
std::vector<uint8_t> gameObjectResponse(uint32_t entry, const std::string& name) {
    Body b;
    b.u32(entry).u32(5).u32(0xD15).cstr(name).cstr("").cstr("").cstr("").cstr("").cstr("").cstr("");
    for (int i = 0; i < 24; ++i) b.u32(0);
    return b.bytes();
}
}  // namespace

TEST_CASE("a player's name is asked for once, and not again once it is known", "[querycache][harness]") {
    GameHarness h;
    h.enterWorld();
    h.game().queryCache().queryPlayerName(kThrall);
    h.game().queryCache().queryPlayerName(kThrall);   // still in flight
    CHECK(sent(h, Opcode::CMSG_NAME_QUERY) == 1);

    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, GameHarness::nameResponseBody(kThrall, "Thrall"));
    h.pump();
    CHECK(h.game().queryCache().lookupName(kThrall) == "Thrall");
    CHECK(h.game().queryCache().lookupPlayerRace(kThrall) == 2);
    CHECK(h.game().queryCache().lookupPlayerClass(kThrall) == 7);

    h.game().queryCache().queryPlayerName(kThrall);
    CHECK(sent(h, Opcode::CMSG_NAME_QUERY) == 1);
}

TEST_CASE("a player who leaves view before their name comes is asked for again on return",
          "[querycache][harness]") {
    GameHarness h;
    h.enterWorld();
    playerComesIntoView(h, kThrall);
    REQUIRE(sent(h, Opcode::CMSG_NAME_QUERY) == 1);

    h.receive(Opcode::SMSG_DESTROY_OBJECT, Body().u64(kThrall).u8(0));
    h.pump();
    playerComesIntoView(h, kThrall);
    CHECK(sent(h, Opcode::CMSG_NAME_QUERY) == 2);
}

TEST_CASE("a name that arrives names the player in view, and the target's frame is told",
          "[querycache][harness]") {
    GameHarness h;
    h.enterWorld();
    playerComesIntoView(h, kThrall);
    h.game().combat().setTarget(kThrall);
    h.clearRecords();

    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, GameHarness::nameResponseBody(kThrall, "Thrall"));
    h.pump();
    CHECK(nameInView(h, kThrall) == "Thrall");
    const auto updates = h.addonEvents("UNIT_NAME_UPDATE");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].args == std::vector<std::string>{"target"});
}

TEST_CASE("a player seen again carries the name already known, without asking",
          "[querycache][harness]") {
    GameHarness h;
    h.enterWorld();
    h.game().queryCache().queryPlayerName(kThrall);
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, GameHarness::nameResponseBody(kThrall, "Thrall"));
    h.pump();

    playerComesIntoView(h, kThrall);
    CHECK(nameInView(h, kThrall) == "Thrall");
    CHECK(sent(h, Opcode::CMSG_NAME_QUERY) == 1);
}

TEST_CASE("a creature template names its units in view and redraws the target's border",
          "[querycache][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kGnoll, ObjectType::UNIT,
                                            {{uf(UF::OBJECT_FIELD_ENTRY), 117},
                                             {uf(UF::UNIT_FIELD_HEALTH), 100},
                                             {uf(UF::UNIT_FIELD_DISPLAYID), 491}}));
    h.pump();
    REQUIRE(sent(h, Opcode::CMSG_CREATURE_QUERY) == 1);
    CHECK(h.game().queryCache().getCreatureRank(117) == -1);
    h.game().combat().setTarget(kGnoll);
    h.clearRecords();

    h.receive(Opcode::SMSG_CREATURE_QUERY_RESPONSE, creatureResponse(117, "Riverpaw Gnoll", 1));
    h.pump();
    CHECK(nameInView(h, kGnoll) == "Riverpaw Gnoll");
    CHECK(h.game().queryCache().getCreatureRank(117) == 1);
    const auto redraws = h.addonEvents("UNIT_CLASSIFICATION_CHANGED");
    REQUIRE(redraws.size() == 1);
    CHECK(redraws[0].args == std::vector<std::string>{"target"});
}

TEST_CASE("a game object template names the objects of it in view", "[querycache][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_UPDATE_OBJECT,
              GameHarness::createObjectBody(kSign, ObjectType::GAMEOBJECT,
                                            {{uf(UF::OBJECT_FIELD_ENTRY), 1234},
                                             {uf(UF::GAMEOBJECT_DISPLAYID), 0xD15}}));
    h.pump();
    REQUIRE(sent(h, Opcode::CMSG_GAMEOBJECT_QUERY) == 1);

    h.receive(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE, gameObjectResponse(1234, "Wanted Poster"));
    h.pump();
    auto sign = h.game().getEntityManager().getEntity(kSign);
    REQUIRE(sign);
    CHECK(std::static_pointer_cast<wowee::game::GameObject>(sign)->getName() == "Wanted Poster");
    REQUIRE(h.game().queryCache().getCachedGameObjectInfo(1234));
}

TEST_CASE("the next character knows none of the last one's names", "[querycache][harness]") {
    GameHarness h;
    h.enterWorld();
    h.game().queryCache().queryPlayerName(kThrall);
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, GameHarness::nameResponseBody(kThrall, "Thrall"));
    h.pump();
    REQUIRE(h.game().queryCache().lookupName(kThrall) == "Thrall");

    PlayerSpec bob;
    bob.guid = 0x11;
    bob.name = "Bob";
    h.relogAs(bob);
    CHECK(h.game().queryCache().lookupName(kThrall).empty());
}
