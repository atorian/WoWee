// /who, end to end: the server's answer, where it goes - the panel or the
// chat - what the panel reads, and the query that goes out.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "who/domain/who_list.hpp"
#include "who/services/who.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

struct Found {
    std::string name;
    std::string guild;
    uint32_t level;
};

/// SMSG_WHO as AzerothCore writes it for WotLK: the count listed, the count
/// online, then each player with a gender byte before the zone.
std::vector<uint8_t> whoAnswer(uint32_t online, const std::vector<Found>& players) {
    Body b;
    b.u32(static_cast<uint32_t>(players.size())).u32(online);
    for (const auto& p : players)
        b.cstr(p.name).cstr(p.guild).u32(p.level).u32(7 /* shaman */).u32(2 /* orc */).u8(0).u32(0);
    return b.bytes();
}

bool chatHas(GameHarness& h, const std::string& text) {
    const auto& history = h.game().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message.find(text) != std::string::npos; });
}

}  // namespace

TEST_CASE("an answer with no panel up is printed in the chat and fills the list", "[who][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_WHO, whoAnswer(40, {{"Ann", "Horde", 80}, {"Bob", "", 72}}));
    h.pump();

    REQUIRE(h.who().getWhoResults().size() == 2);
    CHECK(h.who().getWhoResults()[0].name == "Ann");
    CHECK(h.who().getWhoOnlineCount() == 40);
    CHECK(chatHas(h, "|Hplayer:Ann|h[Ann]|h: Level 80"));
    CHECK(chatHas(h, "<Horde>"));
    CHECK(chatHas(h, "40 |4player:players; total"));
    CHECK(h.addonEvents("WHO_LIST_UPDATE").size() == 1);
}

TEST_CASE("an answer with the who panel up goes to the panel alone", "[who][harness]") {
    GameHarness h;
    h.enterWorld();
    h.who().setWhoToUI(true);
    const size_t linesBefore = h.game().getChatHistory().size();

    h.receive(Opcode::SMSG_WHO, whoAnswer(40, {{"Ann", "", 80}}));
    h.pump();

    CHECK(h.who().getWhoResults().size() == 1);
    CHECK(h.game().getChatHistory().size() == linesBefore);
    CHECK(h.addonEvents("WHO_LIST_UPDATE").size() == 1);
}

TEST_CASE("a search that finds nobody says so on the error line", "[who][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_WHO, whoAnswer(39, {}));
    h.pump();

    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args == std::vector<std::string>{"No players found."});
    CHECK(h.addonEvents("WHO_LIST_UPDATE").empty());
}

TEST_CASE("a query names the player it looks for", "[who][harness]") {
    GameHarness h;
    h.enterWorld();

    h.who().queryWho("Ann");

    auto* sent = h.lastSent(Opcode::CMSG_WHO);
    REQUIRE(sent);
    sent->readUInt32();  // min level
    sent->readUInt32();  // max level
    CHECK(sent->readString() == "Ann");
}

TEST_CASE("another character logging in does not see the last one's answer", "[who][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_WHO, whoAnswer(40, {{"Ann", "", 80}}));
    h.pump();
    REQUIRE_FALSE(h.who().getWhoResults().empty());

    PlayerSpec next;
    next.guid = 0x11;
    next.name = "Second";
    h.relogAs(next);

    CHECK(h.who().getWhoResults().empty());
    CHECK(h.who().getWhoOnlineCount() == 0);
}
