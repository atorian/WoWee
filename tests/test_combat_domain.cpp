// Combat, end to end: what the server says of a fight and what the rest of
// the client is told of it.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"
#include "game/entity.hpp"
#include "game/game_events.hpp"

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
