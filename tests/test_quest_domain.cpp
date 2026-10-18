// Quests, end to end: a quest giver's offer, the player taking it, turning it
// in - the packets, the log the interface reads, and what it is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_events.hpp"
#include "game/update_field_table.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

namespace {

constexpr uint64_t kMarshal = 0xF130000000000197ull;

/// SMSG_QUESTGIVER_QUEST_DETAILS in its short form: the giver, who it informs,
/// the quest, its title, its text and its objectives.
std::vector<uint8_t> offer(uint64_t npc, uint32_t questId, const std::string& title) {
    return Body().u64(npc).u64(0).u32(questId).cstr(title)
        .cstr("The gnolls have overrun the vineyards.").cstr("Kill 10 Riverpaw Gnolls.");
}

bool inLog(GameHarness& h, uint32_t questId) {
    const auto& log = h.game().getQuestLog();
    return std::any_of(log.begin(), log.end(), [&](const auto& q) { return q.questId == questId; });
}

const wowee::game::QuestHandler::QuestLogEntry& logEntry(GameHarness& h, uint32_t questId) {
    const auto& log = h.game().getQuestLog();
    auto it = std::find_if(log.begin(), log.end(), [&](const auto& q) { return q.questId == questId; });
    REQUIRE(it != log.end());
    return *it;
}

/// SMSG_QUEST_QUERY_RESPONSE as AzerothCore's WorldSession::HandleQuestQueryOpcode
/// writes it on 3.3.5a: sixty-five numeric fields, then the strings - title,
/// objectives, details, area description, completion text.
std::vector<uint8_t> questQuery(uint32_t questId, const std::vector<std::string>& strings,
                                uint32_t collectItem = 0, uint32_t collectCount = 0) {
    Body b;
    b.u32(questId).u32(2).u32(50).u32(45).u32(38);   // id, method, level, min level, zone
    b.u32(0).u32(0);                                  // type, suggested players
    b.u32(0).u32(0).u32(0).u32(0);                    // two reputation objectives
    b.u32(0).u32(0);                                  // next in chain, xp id
    b.u32(1000);                                      // money
    b.u32(0).u32(0).u32(0);                           // money at max level, spell, spell cast
    b.u32(0).u32(0);                                  // honour, honour multiplier
    b.u32(0).u32(0).u32(0).u32(0).u32(0).u32(0).u32(0);  // source item .. rep show mask
    b.zeros(4 * 2 * 4);                               // four reward items and counts
    b.zeros(6 * 2 * 4);                               // six reward choices and counts
    b.zeros(5 * 3 * 4);                               // five reputations: ids, values, overrides
    b.u32(0).u32(0).u32(0).u32(0);                    // point of interest
    for (const auto& s : strings) b.cstr(s);
    if (collectItem != 0) {
        // The objectives: four {creature or object, count, dropped item,
        // count} records, then six {required item, count}.
        b.zeros(4 * 4 * 4);
        b.u32(collectItem).u32(collectCount);
        b.zeros(5 * 2 * 4);
    }
    return b.bytes();
}

}  // namespace

TEST_CASE("a quest's query answer names it and states its objective", "[quest][harness]") {
    GameHarness h;
    h.enterWorld();

    // In the log as the server listed it before its text arrived: a number.
    h.receive(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS,
              Body().u64(kMarshal).u64(0).u32(1712).cstr("Quest #1712").cstr("").cstr("").bytes());
    h.pump();
    h.game().acceptQuest();
    REQUIRE(logEntry(h, 1712).title == "Quest #1712");

    h.receive(Opcode::SMSG_QUEST_QUERY_RESPONSE,
              questQuery(1712, {"Bath'rah the Windwatcher",
                                "Bring Bath'rah's Parchment to Bath'rah the Windwatcher.",
                                "The parchment is old and the ink has faded.",
                                "",
                                "Return to Bath'rah."}));
    h.pump();

    // The strings start after the sixty-five fields. Read from fifty-seven -
    // which this did - the title came back empty out of the reward block and
    // was guessed from the shape of whatever text came next.
    CHECK(logEntry(h, 1712).title == "Bath'rah the Windwatcher");
    CHECK(logEntry(h, 1712).objectives == "Bring Bath'rah's Parchment to Bath'rah the Windwatcher.");
}

TEST_CASE("a quest taken from its giver is in the log and told to the server", "[quest][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<uint32_t> accepted;
    auto sub = h.events().subscribe<ev::QuestAccepted>(
        [&](const ev::QuestAccepted& e) { accepted.push_back(e.questId); });

    h.receive(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, offer(kMarshal, 11, "Riverpaw Gnoll Bounty"));
    h.pump();
    CHECK(h.addonEvents("QUEST_DETAIL").size() == 1);

    h.game().acceptQuest();

    auto* sent = h.lastSent(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST);
    REQUIRE(sent != nullptr);
    CHECK(sent->readUInt64() == kMarshal);
    CHECK(sent->readUInt32() == 11);
    CHECK(accepted == std::vector<uint32_t>{11});
    REQUIRE(inLog(h, 11));
    const auto& log = h.game().getQuestLog();
    CHECK(std::find_if(log.begin(), log.end(), [](const auto& q) { return q.questId == 11; })->title ==
          "Riverpaw Gnoll Bounty");
}

TEST_CASE("a quest turned in leaves the log and is announced by its title", "[quest][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, offer(kMarshal, 11, "Riverpaw Gnoll Bounty"));
    h.pump();
    h.game().acceptQuest();
    REQUIRE(inLog(h, 11));

    std::vector<ev::QuestCompleted> completed;
    auto sub = h.events().subscribe<ev::QuestCompleted>(
        [&](const ev::QuestCompleted& e) { completed.push_back(e); });

    // The quest, then what it paid: experience, money, honour, talents,
    // arena points and the reward items, none here.
    h.receive(Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE, Body().u32(11).u32(450).u32(1200).zeros(16));
    h.pump();

    REQUIRE(completed.size() == 1);
    CHECK(completed[0].questId == 11);
    CHECK(completed[0].title == "Riverpaw Gnoll Bounty");
    CHECK_FALSE(inLog(h, 11));
}

TEST_CASE("a full quest log refuses a quest before asking the server", "[quest][harness]") {
    GameHarness h;
    h.enterWorld();

    // A WotLK log holds twenty-five.
    for (uint32_t id = 100; id < 125; ++id) {
        h.receive(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, offer(kMarshal, id, "Quest " + std::to_string(id)));
        h.pump();
        h.game().acceptQuest();
    }
    REQUIRE(h.game().getQuestLog().size() == 25);
    h.clearRecords();

    size_t errors = 0;
    auto sub = h.events().subscribe<ev::UiErrorRaised>([&](const ev::UiErrorRaised&) { ++errors; });
    h.receive(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, offer(kMarshal, 200, "One Too Many"));
    h.pump();
    h.game().acceptQuest();

    CHECK(h.countSent(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST) == 0);
    CHECK_FALSE(inLog(h, 200));
    CHECK(errors == 1);
}

TEST_CASE("what the bags hold is what a quest's collect objective counts",
          "[quest][inventory][harness]") {
    // 3.3.5a never sends a collect objective's count: the quest log counts it
    // from the bags, which are the inventory's. Across the two domains on the
    // bus, so a log reading a copy of the bags nobody fills shows 0/5 here.
    constexpr uint32_t kQuest = 33;
    constexpr uint32_t kWolfMeat = 750;
    constexpr uint64_t kMeat = 0x4000000000000B01ull;
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.receive(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, offer(kMarshal, kQuest, "Wolves Across the Border"));
    h.pump();
    h.game().acceptQuest();
    h.receive(Opcode::SMSG_QUEST_QUERY_RESPONSE,
              questQuery(kQuest, {"Wolves Across the Border", "Bring 5 Tough Wolf Meat.",
                                  "The wolves have been raiding the farms.", "", "Return."},
                         kWolfMeat, 5));
    h.pump();
    REQUIRE(logEntry(h, kQuest).itemObjectives[0].itemId == kWolfMeat);

    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::createItemBody(kMeat, kWolfMeat, 3));
    GameHarness::Fields backpack;
    GameHarness::putGuid(backpack, wowee::game::fieldIndex(wowee::game::UF::PLAYER_FIELD_PACK_SLOT_1), kMeat);
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(h.player().guid, backpack));
    h.pump();

    const auto& counts = logEntry(h, kQuest).itemCounts;
    REQUIRE(counts.count(kWolfMeat) == 1);
    CHECK(counts.at(kWolfMeat) == 3);
    CHECK_FALSE(h.addonEvents("QUEST_WATCH_UPDATE").empty());
}

TEST_CASE("the next character is offered none of the last one's quests", "[quest][harness]") {
    wowee::test::PlayerSpec alice;
    alice.guid = 0x61;
    alice.name = "Alice";
    wowee::test::PlayerSpec bob;
    bob.guid = 0x62;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    // A party member shares a quest (SMSG_QUEST_CONFIRM_ACCEPT: the quest,
    // its title, the sharer), a quest giver's page is open, and Alice tracks
    // a quest.
    h.receive(Opcode::SMSG_QUEST_CONFIRM_ACCEPT, Body().u32(11).cstr("Riverpaw Gnoll Bounty").u64(0x99));
    h.receive(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS, offer(kMarshal, 12, "The Fargodeep Mine"));
    h.pump();
    h.game().setQuestTracked(11, true);
    REQUIRE(h.game().hasPendingSharedQuest());
    REQUIRE(h.game().isQuestDetailsOpen());

    h.relogAs(bob);

    // Bob was asked to accept a quest shared with Alice, saw the page her
    // quest giver had open, and tracked her quest.
    CHECK_FALSE(h.game().hasPendingSharedQuest());
    CHECK_FALSE(h.game().isQuestDetailsOpen());
    CHECK_FALSE(h.game().isQuestTracked(11));
}
