// Achievements: the catalog's category tree, the player's progress, and the
// service against fake game files and the real event bus. Only
// wowee_achievement is linked.
#include <catch_amalgamated.hpp>

#include "achievement/services/achievement_service.hpp"
#include "core/event_bus.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace wowee::achievement;

namespace {

AchievementRecord row(uint32_t id, const std::string& title, uint32_t category, uint32_t flags = 0,
                      uint32_t points = 0, uint32_t supercedes = 0) {
    AchievementRecord r;
    r.id = id;
    r.title = title;
    r.category = category;
    r.flags = flags;
    r.points = points;
    r.supercedes = supercedes;
    return r;
}

CategoryRecord category(uint32_t id, uint32_t parent, const std::string& name, uint32_t uiOrder) {
    return CategoryRecord{id, parent, name, uiOrder};
}

CriterionRecord criterion(uint32_t achievementId, uint32_t id, uint32_t timeLimit = 0) {
    CriterionRecord r;
    r.achievementId = achievementId;
    r.criterion.id = id;
    r.criterion.timeLimit = timeLimit;
    return r;
}

template <typename E>
const E* find(const std::vector<AchievementChange>& changes) {
    for (const auto& c : changes)
        if (const auto* e = std::get_if<E>(&c)) return e;
    return nullptr;
}

struct FakeFiles final : IAchievementData {
    bool assets = true;
    AchievementTable table;
    std::vector<CategoryRecord> tree;
    std::vector<CriterionRecord> rows;
    bool ready() const override { return assets; }
    AchievementTable achievements() override { return table; }
    std::vector<CategoryRecord> categories() override { return tree; }
    std::vector<CriterionRecord> criteria() override { return rows; }
};

constexpr uint32_t kTop = 0xFFFFFFFFu;

} // namespace

TEST_CASE("A category holding statistics, and every one above it, goes to the statistics tab", "[achievement]") {
    AchievementCatalog catalog;
    const std::vector<AchievementRecord> rows = {
        row(100, "Deaths", 3, /*statistic*/ 0x1),
        row(101, "Level 10", 5),
    };
    catalog.describe(rows);
    catalog.place(rows, {
        category(1, kTop, "General", 1),
        category(2, kTop, "Statistics", 10),
        category(3, 2, "Character", 1),
        category(4, 1, "Quests", 2),
        category(5, 1, "Leveling", 1),
    });
    // Ordered by ui order, id breaking ties, and split into the two tabs.
    CHECK(catalog.categoryOrder() == std::vector<uint32_t>{1, 5, 4});
    CHECK(catalog.statisticCategoryOrder() == std::vector<uint32_t>{3, 2});
    REQUIRE(catalog.category(3) != nullptr);
    CHECK(catalog.category(3)->parentId == 2);
    CHECK(catalog.category(1)->parentId == -1);
}

TEST_CASE("An achievement with no title is not placed in its category", "[achievement]") {
    AchievementCatalog catalog;
    const std::vector<AchievementRecord> rows = {row(100, "Level 10", 5), row(101, "", 5)};
    catalog.describe(rows);
    catalog.place(rows, {category(5, kTop, "Leveling", 1)});
    CHECK(catalog.achievementsIn(5) == std::vector<uint32_t>{100});
    CHECK(catalog.categoryOf(101) == 0);
}

TEST_CASE("A supercedes chain is walked both ways", "[achievement]") {
    AchievementCatalog catalog;
    const std::vector<AchievementRecord> rows = {row(6, "Level 10", 5), row(7, "Level 20", 5, 0, 10, 6)};
    catalog.describe(rows);
    catalog.place(rows, {});
    CHECK(catalog.supercedes(7) == 6);
    CHECK(catalog.supercededBy(6) == 7);
    CHECK(catalog.supercededBy(7) == 0);
}

TEST_CASE("A criteria update that moves nothing raises nothing, unless it is timed", "[achievement]") {
    Achievements player;
    auto first = player.progress(500, 3, 0, {});
    REQUIRE(find<CriteriaProgressed>(first) != nullptr);
    CHECK(find<CriteriaProgressed>(first)->progress == 3);
    CHECK(player.progress(500, 3, 0, {}).empty());

    auto timed = player.progress(500, 3, 40, {.achievementId = 1832, .timeLimit = 420});
    CHECK(find<CriteriaProgressed>(timed) == nullptr);
    REQUIRE(find<TimedCriteriaUpdated>(timed) != nullptr);
    CHECK(find<TimedCriteriaUpdated>(timed)->achievementId == 1832);
    CHECK(find<TimedCriteriaUpdated>(timed)->elapsed == 40);
    CHECK(find<TimedCriteriaUpdated>(timed)->timeLimit == 420);
}

TEST_CASE("The login list replaces what was known", "[achievement]") {
    Achievements player;
    player.earn(6, 111);
    player.progress(500, 3, 0, {});
    player.replaceAll({{7, 222}}, {{501, 9}});
    CHECK(player.earned() == std::unordered_set<uint32_t>{7});
    CHECK(player.dateOf(6) == 0);
    CHECK(player.dateOf(7) == 222);
    CHECK(player.criteria().count(500) == 0);
    CHECK(player.criteria().at(501) == 9);
}

TEST_CASE("A revoked achievement loses its date too", "[achievement]") {
    Achievements player;
    player.earn(6, 111);
    player.revoke(6);
    CHECK(player.earned().empty());
    CHECK(player.dateOf(6) == 0);
}

TEST_CASE("Asking for names before the files can be read does not give up on them", "[achievement]") {
    FakeFiles files;
    files.assets = false;
    files.table.rows = {row(6, "Level 10", 5)};
    wowee::core::EventBus bus;
    AchievementService service{files, bus};
    service.ensureAchievementNamesLoaded();
    CHECK(service.getAchievementName(6).empty());
    files.assets = true;
    service.ensureAchievementNamesLoaded();
    CHECK(service.getAchievementName(6) == "Level 10");
    CHECK(service.getAchievementCount() == 1);
}

TEST_CASE("Achievements are not placed when Achievement.dbc cannot say where", "[achievement]") {
    FakeFiles files;
    files.table.rows = {row(6, "Level 10", 5)};
    files.table.hasCategories = false;
    files.tree = {category(5, kTop, "Leveling", 1)};
    wowee::core::EventBus bus;
    AchievementService service{files, bus};
    service.ensureAchievementCategoriesLoaded();
    CHECK(service.getAchievementName(6) == "Level 10");
    CHECK(service.getCategoryAchievements(5).empty());
    CHECK(service.getAchievementCategoryOrder().empty());
}

TEST_CASE("Points add up over what the player has earned", "[achievement]") {
    FakeFiles files;
    files.table.rows = {row(6, "Level 10", 5, 0, 10), row(7, "Level 20", 5, 0, 15), row(8, "Level 30", 5, 0, 20)};
    wowee::core::EventBus bus;
    AchievementService service{files, bus};
    service.onEarned(6, 1);
    service.onEarned(8, 2);
    CHECK(service.getTotalAchievementPoints() == 30);
}

TEST_CASE("Earning one is announced, and its name is known by then", "[achievement]") {
    FakeFiles files;
    files.table.rows = {row(6, "Level 10", 5)};
    wowee::core::EventBus bus;
    AchievementService service{files, bus};
    std::vector<std::string> seen;
    auto sub = bus.subscribe<AchievementEarned>([&](const AchievementEarned& e) {
        seen.push_back(std::to_string(e.achievementId) + " " + service.getAchievementName(e.achievementId));
    });
    service.onEarned(6, 123);
    CHECK(seen == std::vector<std::string>{"6 Level 10"});
    CHECK(service.getAchievementDate(6) == 123);
}

TEST_CASE("A timed criteria's window comes from Achievement_Criteria.dbc", "[achievement]") {
    FakeFiles files;
    files.rows = {criterion(1832, 500, 420)};
    wowee::core::EventBus bus;
    AchievementService service{files, bus};
    std::vector<TimedCriteriaUpdated> timed;
    auto sub = bus.subscribe<TimedCriteriaUpdated>([&](const TimedCriteriaUpdated& e) { timed.push_back(e); });
    service.onCriteriaProgress(500, 1, 30);
    REQUIRE(timed.size() == 1);
    CHECK(timed[0].achievementId == 1832);
    CHECK(timed[0].timeLimit == 420);
    CHECK(timed[0].elapsed == 30);
}

TEST_CASE("An inspected player's achievements are kept by guid until the next character logs in",
          "[achievement]") {
    FakeFiles files;
    wowee::core::EventBus bus;
    AchievementService service{files, bus};
    int ready = 0;
    auto sub = bus.subscribe<InspectAchievementsReady>([&](const auto&) { ++ready; });
    service.onInspected(42, {{6, 111}});
    CHECK(ready == 1);
    REQUIRE(service.getInspectedPlayerAchievements(42) != nullptr);
    CHECK(service.getInspectedPlayerAchievements(42)->at(6) == 111);
    CHECK(service.getInspectedPlayerAchievements(43) == nullptr);
    service.beginCharacter();
    CHECK(service.getInspectedPlayerAchievements(42) == nullptr);
}

TEST_CASE("The next character has none of the last one's achievements, counters or tracked set",
          "[achievement]") {
    FakeFiles files;
    files.table.rows = {row(6, "Level 10", 5, 0, 10)};
    wowee::core::EventBus bus;
    AchievementService service{files, bus};
    service.onAllData({{6, 99}}, {{500, 3}});
    service.setAchievementTracked(6, true);
    service.setAchievementComparisonGuid(42);

    service.beginCharacter();

    CHECK(service.getEarnedAchievements().empty());
    CHECK(service.getAchievementDate(6) == 0);
    CHECK(service.getCriteriaProgress().empty());
    CHECK(service.getTrackedAchievements().empty());
    CHECK(service.getAchievementComparisonGuid() == 0);
    CHECK(service.getTotalAchievementPoints() == 0);
    // What the files say is the game's, not the character's.
    CHECK(service.getAchievementName(6) == "Level 10");
}
