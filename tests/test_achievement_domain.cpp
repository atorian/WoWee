// Achievements, end to end: the realm's achievement packets, what the
// achievement panel, the tracker and the chat are told, and what they read
// back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "achievement/domain/catalog.hpp"
#include "achievement/services/achievements.hpp"
#include "game/game_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kStranger = 0x0000000000000099ull;
constexpr uint32_t kLevel10 = 6;
constexpr uint32_t kLevel20 = 7;
constexpr uint32_t kSentinel = 0xFFFFFFFFu;
constexpr uint32_t kEarnedOn = 0x0A1B2C3D;

/// Achievement.dbc as a test writes it: these rows, with their titles.
void achievementFiles(GameHarness& h) {
    wowee::achievement::AchievementRecord ten;
    ten.id = kLevel10;
    ten.title = "Level 10";
    ten.points = 10;
    wowee::achievement::AchievementRecord twenty;
    twenty.id = kLevel20;
    twenty.title = "Level 20";
    twenty.points = 10;
    h.achievementFiles().table.rows = {ten, twenty};
}

/// One criteria's progress after its id, as the realm writes it in
/// SMSG_CRITERIA_UPDATE and in the login list: the counter and the player as
/// packed guids, then the flags, the date and the seconds since.
Body& criteriaTail(Body& b, uint64_t counter, uint64_t player, uint32_t flags, uint32_t elapsed) {
    return b.packedGuid(counter).packedGuid(player).u32(flags).u32(kEarnedOn).u32(elapsed).u32(elapsed);
}

std::vector<uint8_t> criteriaUpdate(uint32_t criteriaId, uint64_t counter, uint64_t player,
                                    uint32_t flags = 0, uint32_t elapsed = 0) {
    Body b;
    b.u32(criteriaId);
    return criteriaTail(b, counter, player, flags, elapsed).bytes();
}

size_t linesSaying(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return static_cast<size_t>(std::count_if(history.begin(), history.end(),
                                              [&](const auto& line) { return line.message == text; }));
}

}  // namespace

TEST_CASE("the login list is what the panel reads, and it is told the list arrived", "[achievement][harness]") {
    GameHarness h;
    h.enterWorld();

    Body b;
    b.u32(kLevel10).u32(kEarnedOn).u32(kSentinel);
    b.u32(500);
    criteriaTail(b, 3, h.player().guid, 0, 0);
    b.u32(kSentinel);
    h.receive(Opcode::SMSG_ALL_ACHIEVEMENT_DATA, b.bytes());
    h.pump();

    const auto& achievements = h.achievements();
    CHECK(achievements.getEarnedAchievements() == std::unordered_set<uint32_t>{kLevel10});
    CHECK(achievements.getAchievementDate(kLevel10) == kEarnedOn);
    REQUIRE(achievements.getCriteriaProgress().count(500) == 1);
    CHECK(achievements.getCriteriaProgress().at(500) == 3);
    CHECK(h.addonEvents("RECEIVED_ACHIEVEMENT_LIST").size() == 1);
}

TEST_CASE("criteria of every counter width in the login list are all kept", "[achievement][harness]") {
    // The failure a fixed eight-byte counter read produced was never one wrong
    // number: the loop lost its place and read the rest of the packet as
    // records. Three records of different packed widths, then the sentinel.
    GameHarness h;
    h.enterWorld();

    Body b;
    b.u32(kSentinel);  // no achievements
    b.u32(100);
    criteriaTail(b, 3, h.player().guid, 0, 0);
    b.u32(200);
    criteriaTail(b, 0x1234, h.player().guid, 0, 0);
    b.u32(300);
    criteriaTail(b, 0xFFFFFFFFFFull, h.player().guid, 0, 0);
    b.u32(kSentinel);
    h.receive(Opcode::SMSG_ALL_ACHIEVEMENT_DATA, b.bytes());
    h.pump();

    const auto& progress = h.achievements().getCriteriaProgress();
    REQUIRE(progress.size() == 3);
    REQUIRE(progress.count(100) == 1);
    REQUIRE(progress.count(200) == 1);
    REQUIRE(progress.count(300) == 1);
    CHECK(progress.at(100) == 3);
    CHECK(progress.at(200) == 0x1234);
    CHECK(progress.at(300) == 0xFFFFFFFFFFull);
}

TEST_CASE("an achievement the player earns is kept and announced, another's is not", "[achievement][harness]") {
    GameHarness h;
    achievementFiles(h);
    h.enterWorld();

    // SMSG_ACHIEVEMENT_EARNED: who (packed), the achievement, the packed date.
    h.receive(Opcode::SMSG_ACHIEVEMENT_EARNED, Body().packedGuid(kStranger).u32(kLevel10).u32(kEarnedOn));
    h.receive(Opcode::SMSG_ACHIEVEMENT_EARNED, Body().packedGuid(h.player().guid).u32(kLevel20).u32(kEarnedOn));
    h.pump();

    CHECK(h.achievements().getEarnedAchievements() == std::unordered_set<uint32_t>{kLevel20});
    CHECK(h.achievements().getAchievementDate(kLevel20) == kEarnedOn);
    const auto earned = h.addonEvents("ACHIEVEMENT_EARNED");
    REQUIRE(earned.size() == 1);
    CHECK(earned[0].args == std::vector<std::string>{"7"});
    // Named from Achievement.dbc. Somebody else's comes as CHAT_MSG_ACHIEVEMENT.
    CHECK(linesSaying(h, "Achievement earned: Level 20") == 1);
    CHECK(linesSaying(h, "Achievement earned: Level 10") == 0);
}

TEST_CASE("an achievement the files do not name is announced by its id", "[achievement][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_ACHIEVEMENT_EARNED, Body().packedGuid(h.player().guid).u32(1832).u32(kEarnedOn));
    h.pump();

    CHECK(linesSaying(h, "Achievement earned! (ID 1832)") == 1);
}

TEST_CASE("a criteria that moved is told to the tracker, and one that did not is not", "[achievement][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_CRITERIA_UPDATE, criteriaUpdate(500, 3, h.player().guid));
    h.receive(Opcode::SMSG_CRITERIA_UPDATE, criteriaUpdate(500, 3, h.player().guid));
    h.pump();
    auto updates = h.addonEvents("CRITERIA_UPDATE");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].args == std::vector<std::string>{"500", "3"});
    CHECK(h.achievements().getCriteriaProgress().at(500) == 3);

    // A timed criteria whose window ran out reads as zero.
    h.receive(Opcode::SMSG_CRITERIA_UPDATE, criteriaUpdate(500, 3, h.player().guid, 1));
    h.pump();
    updates = h.addonEvents("CRITERIA_UPDATE");
    REQUIRE(updates.size() == 2);
    CHECK(updates[1].args == std::vector<std::string>{"500", "0"});
    CHECK(h.addonEvents("TRACKED_ACHIEVEMENT_UPDATE").empty());
}

TEST_CASE("a timed criteria tells the tracker how far into its window the player is", "[achievement][harness]") {
    GameHarness h;
    wowee::achievement::CriterionRecord timed;
    timed.achievementId = 1832;
    timed.criterion.id = 600;
    timed.criterion.timeLimit = 420;
    h.achievementFiles().criterionRows = {timed};
    h.enterWorld();

    h.receive(Opcode::SMSG_CRITERIA_UPDATE, criteriaUpdate(600, 1, h.player().guid, 0, 30));
    h.pump();

    const auto tracked = h.addonEvents("TRACKED_ACHIEVEMENT_UPDATE");
    REQUIRE(tracked.size() == 1);
    CHECK(tracked[0].args == std::vector<std::string>{"1832", "600", "30", "420"});
    CHECK(h.firedAt("CRITERIA_UPDATE") < h.firedAt("TRACKED_ACHIEVEMENT_UPDATE"));
}

TEST_CASE("an achievement or criteria the realm takes back is gone", "[achievement][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_ACHIEVEMENT_EARNED, Body().packedGuid(h.player().guid).u32(kLevel10).u32(kEarnedOn));
    h.receive(Opcode::SMSG_CRITERIA_UPDATE, criteriaUpdate(500, 3, h.player().guid));
    h.pump();

    h.receive(Opcode::SMSG_ACHIEVEMENT_DELETED, Body().u32(kLevel10).bytes());
    h.receive(Opcode::SMSG_CRITERIA_DELETED, Body().u32(500).bytes());
    h.pump();

    CHECK(h.achievements().getEarnedAchievements().empty());
    CHECK(h.achievements().getAchievementDate(kLevel10) == 0);
    CHECK(h.achievements().getCriteriaProgress().count(500) == 0);
}

TEST_CASE("an inspected player's achievements are kept with their dates, and the comparison tab told",
          "[achievement][harness]") {
    GameHarness h;
    h.enterWorld();

    Body b;
    b.packedGuid(kStranger);
    b.u32(kLevel10).u32(kEarnedOn).u32(kSentinel);
    b.u32(500).u64(3).u32(0).u32(0).u32(kSentinel);   // their criteria, walked past
    h.receive(Opcode::SMSG_RESPOND_INSPECT_ACHIEVEMENTS, b.bytes());
    h.pump();

    const auto* theirs = h.achievements().getInspectedPlayerAchievements(kStranger);
    REQUIRE(theirs);
    REQUIRE(theirs->size() == 1);
    CHECK(theirs->at(kLevel10) == kEarnedOn);
    CHECK(h.addonEvents("INSPECT_ACHIEVEMENT_READY").size() == 1);
    CHECK(h.achievements().getEarnedAchievements().empty());
}

TEST_CASE("a realm first is said with the achievement's name, or its number", "[achievement][harness]") {
    GameHarness h;
    achievementFiles(h);
    h.enterWorld();

    h.receive(Opcode::SMSG_SERVER_FIRST_ACHIEVEMENT, Body().cstr("Jaina").u64(kStranger).u32(kLevel20).bytes());
    h.receive(Opcode::SMSG_SERVER_FIRST_ACHIEVEMENT, Body().cstr("Thrall").u64(kStranger).u32(1832).bytes());
    h.pump();

    CHECK(linesSaying(h, "Jaina is the first on the realm to earn: Level 20!") == 1);
    CHECK(linesSaying(h, "Thrall is the first on the realm to earn achievement #1832!") == 1);
}

TEST_CASE("the next character has none of the last one's achievements, tracked set or comparison",
          "[achievement][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_ACHIEVEMENT_EARNED, Body().packedGuid(alice.guid).u32(kLevel10).u32(kEarnedOn));
    h.receive(Opcode::SMSG_CRITERIA_UPDATE, criteriaUpdate(500, 3, alice.guid));
    Body inspect;
    inspect.packedGuid(kStranger).u32(kLevel20).u32(kEarnedOn).u32(kSentinel).u32(kSentinel);
    h.receive(Opcode::SMSG_RESPOND_INSPECT_ACHIEVEMENTS, inspect.bytes());
    h.pump();
    h.achievements().setAchievementTracked(kLevel20, true);
    h.achievements().setAchievementComparisonGuid(kStranger);
    REQUIRE_FALSE(h.achievements().getEarnedAchievements().empty());

    h.relogAs(bob);

    CHECK(h.achievements().getEarnedAchievements().empty());
    CHECK(h.achievements().getCriteriaProgress().empty());
    CHECK(h.achievements().getTrackedAchievements().empty());
    CHECK(h.achievements().getAchievementComparisonGuid() == 0);
    CHECK(h.achievements().getInspectedPlayerAchievements(kStranger) == nullptr);
}
