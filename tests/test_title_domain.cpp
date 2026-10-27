// Titles, end to end: the realm's title packets and the worn title on the
// player's fields, what the character sheet and the chat are told, and the
// request to wear one.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "title/services/titles.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::game::UF;
using wowee::game::fieldIndex;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint32_t kExplorer = 47;

/// CharTitles.dbc with one title: "the Explorer", worn at bit 47.
void titleFiles(GameHarness& h) {
    h.titleFiles().rows = {{.id = 46, .bit = kExplorer, .format = "%s the Explorer"}};
}

/// SMSG_NAME_QUERY_RESPONSE: the player's own name, which is what a title is
/// formatted with.
void nameKnown(GameHarness& h, const std::string& name) {
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE,
              Body().packedGuid(h.player().guid).u8(0).cstr(name).cstr("").u8(2).u8(0).u8(7).bytes());
    h.pump();
}

std::vector<uint8_t> titleEarned(uint32_t bit, bool lost) {
    return Body().u32(bit).u32(lost ? 1 : 0).bytes();
}

size_t linesSaying(GameHarness& h, const std::string& text) {
    const auto& history = h.game().getChatHistory();
    return static_cast<size_t>(std::count_if(history.begin(), history.end(),
                                              [&](const auto& line) { return line.message == text; }));
}

}  // namespace

TEST_CASE("a title earned is known, the character sheet is told, and the chat names it",
          "[title][harness]") {
    GameHarness h;
    titleFiles(h);
    PlayerSpec alice;
    alice.name = "Alice";
    h.enterWorld(alice);

    // The line speaks to the player, so before the name is known it is "you".
    h.receive(Opcode::SMSG_TITLE_EARNED, titleEarned(kExplorer, false));
    h.pump();

    CHECK(h.titles().getKnownTitleBits() == std::unordered_set<uint32_t>{kExplorer});
    CHECK(h.addonEvents("KNOWN_TITLES_UPDATE").size() == 1);
    CHECK(linesSaying(h, "Title earned: you the Explorer!") == 1);

    nameKnown(h, "Alice");

    h.receive(Opcode::SMSG_TITLE_EARNED, titleEarned(kExplorer, true));
    h.pump();

    CHECK(h.titles().getKnownTitleBits().empty());
    CHECK(h.addonEvents("KNOWN_TITLES_UPDATE").size() == 2);
    CHECK(linesSaying(h, "Title removed: Alice the Explorer.") == 1);
}

TEST_CASE("a title the files do not name is said by its bit", "[title][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_TITLE_EARNED, titleEarned(99, false));
    h.pump();

    CHECK(h.titles().getKnownTitleBits().count(99) == 1);
    CHECK(linesSaying(h, "Title earned (bit 99)!") == 1);
}

TEST_CASE("the title worn is the one on the player's fields", "[title][harness]") {
    GameHarness h;
    titleFiles(h);
    PlayerSpec alice;
    alice.name = "Alice";
    h.enterWorld(alice);
    h.playerInView();
    nameKnown(h, "Alice");
    CHECK(h.titles().getChosenTitleBit() == -1);

    GameHarness::Fields chosen{{fieldIndex(UF::PLAYER_CHOSEN_TITLE), kExplorer}};
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(h.player().guid, chosen));
    h.pump();

    CHECK(h.titles().getChosenTitleBit() == static_cast<int32_t>(kExplorer));
    CHECK(h.titles().getFormattedTitle(kExplorer) == "Alice the Explorer");
}

TEST_CASE("wearing a title asks the realm for it and is remembered", "[title][harness]") {
    GameHarness h;
    h.enterWorld();

    h.titles().sendSetTitle(static_cast<int32_t>(kExplorer));

    auto* request = h.lastSent(Opcode::CMSG_SET_TITLE);
    REQUIRE(request);
    CHECK(request->readUInt32() == kExplorer);
    CHECK(h.titles().getChosenTitleBit() == static_cast<int32_t>(kExplorer));
}

TEST_CASE("the next character knows and wears none of the last one's titles", "[title][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_TITLE_EARNED, titleEarned(kExplorer, false));
    h.pump();
    h.titles().sendSetTitle(static_cast<int32_t>(kExplorer));
    REQUIRE_FALSE(h.titles().getKnownTitleBits().empty());

    h.relogAs(bob);

    CHECK(h.titles().getKnownTitleBits().empty());
    CHECK(h.titles().getChosenTitleBit() == -1);
}
