// Titles: what CharTitles.dbc says, what the player has and wears, and the
// service against fake files, a recording gateway and the real event bus.
// Only wowee_title is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "title/services/title_service.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace wowee::title;

namespace {

struct FakeDbc final : ITitleData {
    bool assets = true;
    std::vector<TitleRecord> rows = {
        {.id = 46, .bit = 47, .format = "%s the Explorer"},
        {.id = 1, .bit = 1, .format = "Private %s"},
        {.id = 9, .bit = 0, .format = "%s of the Nightfall"},
    };
    bool ready() const override { return assets; }
    std::vector<TitleRecord> titles() override { return rows; }
};

struct RecordingGateway final : ITitleGateway {
    bool world = true;
    std::string name = "Arthas";
    std::vector<int32_t> worn;
    bool online() const override { return world; }
    std::string playerName() const override { return name; }
    void sendSetTitle(int32_t bit) override { worn.push_back(bit); }
};

} // namespace

TEST_CASE("The name goes where the title says", "[title]") {
    CHECK(formatTitle("%s the Explorer", "Arthas") == "Arthas the Explorer");
    CHECK(formatTitle("Private %s", "Arthas") == "Private Arthas");
    CHECK(formatTitle("Champion", "Arthas") == "Champion");
}

TEST_CASE("A title is found by its bit or by the id a quest reward carries", "[title]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    TitleService service{dbc, gateway, bus};
    CHECK(service.getFormattedTitle(47) == "Arthas the Explorer");
    CHECK(service.getFormattedTitleById(46) == "Arthas the Explorer");
    CHECK(service.getFormattedTitleById(9) == "Arthas of the Nightfall");
    CHECK(service.getFormattedTitleFor(1, "Jaina") == "Private Jaina");
    CHECK(service.getFormattedTitle(9).empty());
    // A title that cannot be worn has no bit, and bit zero is not one.
    CHECK(service.getFormattedTitle(0).empty());
}

TEST_CASE("A player whose name is not known yet is 'unknown'", "[title]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    gateway.name.clear();
    wowee::core::EventBus bus;
    TitleService service{dbc, gateway, bus};
    CHECK(service.getFormattedTitle(1) == "Private unknown");
}

TEST_CASE("Asking before CharTitles.dbc can be read does not give up on it", "[title]") {
    FakeDbc dbc;
    dbc.assets = false;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    TitleService service{dbc, gateway, bus};
    CHECK(service.getFormattedTitle(47).empty());
    dbc.assets = true;
    CHECK(service.getFormattedTitle(47) == "Arthas the Explorer");
}

TEST_CASE("Earning and losing a title updates the known set and is announced", "[title]") {
    Titles titles;
    auto earned = titles.earned(47, false);
    CHECK(titles.known() == std::unordered_set<uint32_t>{47});
    REQUIRE(earned.size() == 1);
    CHECK(std::holds_alternative<KnownTitlesChanged>(earned[0]));
    titles.earned(47, true);
    CHECK(titles.known().empty());
}

TEST_CASE("A title earned is announced after the known set, named for the player", "[title]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    TitleService service{dbc, gateway, bus};
    std::vector<std::string> seen;
    auto known = bus.subscribe<KnownTitlesChanged>([&](const auto&) { seen.push_back("known"); });
    auto told = bus.subscribe<TitleEarned>([&](const TitleEarned& e) {
        seen.push_back(std::to_string(e.bit) + (e.lost ? " lost " : " earned ") + e.title);
    });

    service.onEarned(47, false);
    gateway.name.clear();
    service.onEarned(1, true);
    service.onEarned(99, false);

    CHECK(seen == std::vector<std::string>{"known", "47 earned Arthas the Explorer",
                                           "known", "1 lost Private you",
                                           "known", "99 earned "});
}

TEST_CASE("A server's title list adds, removes or replaces", "[title]") {
    Titles titles;
    titles.updated(1, {1, 2, 3});
    titles.updated(2, {2});
    CHECK(titles.known() == std::unordered_set<uint32_t>{1, 3});
    titles.updated(3, {7});
    CHECK(titles.known() == std::unordered_set<uint32_t>{7});
    CHECK(titles.updated(4, {9}).empty());
    CHECK(titles.known() == std::unordered_set<uint32_t>{7});
}

TEST_CASE("Wearing a title needs the world and is remembered", "[title]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    TitleService service{dbc, gateway, bus};
    service.sendSetTitle(47);
    gateway.world = false;
    service.sendSetTitle(1);
    CHECK(gateway.worn == std::vector<int32_t>{47});
    CHECK(service.getChosenTitleBit() == 47);
    service.onChosen(-1);
    CHECK(service.getChosenTitleBit() == -1);
}

TEST_CASE("Leaving the world forgets the titles known and the one worn", "[title]") {
    FakeDbc dbc;
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    TitleService service{dbc, gateway, bus};
    service.onEarned(47, false);
    service.onChosen(47);
    service.leftWorld();
    CHECK(service.getKnownTitleBits().empty());
    CHECK(service.getChosenTitleBit() == -1);
    // What CharTitles.dbc says is the game's, not the character's.
    CHECK(service.getFormattedTitle(47) == "Arthas the Explorer");
}
