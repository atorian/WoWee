// Quests, end to end: a quest giver's offer, the player taking it, turning it
// in - the packets, the log the interface reads, and what it is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_events.hpp"

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

}  // namespace

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
