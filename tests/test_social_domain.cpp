// The guild, end to end: the server's packets, what the interface reads and
// is told.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;

TEST_CASE("the guild bank's balance event is the bank's figure, not a chat line",
          "[social][harness]") {
    GameHarness h;
    h.enterWorld();
    const size_t linesBefore = h.game().getChatHistory().size();

    // Type 17, one string: the balance as sixteen zero-padded hex digits,
    // broadcast to every member whenever anyone touches the bank. Unhandled,
    // it reached guild chat as "Guild event 17: 000000000051451B".
    h.receive(Opcode::SMSG_GUILD_EVENT, Body().u8(17).u8(1).cstr("000000000051451B"));
    h.pump();

    CHECK(h.game().getChatHistory().size() == linesBefore);
    CHECK(h.addonEvents("CHAT_MSG_SYSTEM").empty());
    CHECK(h.addonEvents("CHAT_MSG_GUILD").empty());
    // 532g 61s 7c, read as hex.
    CHECK(h.game().getGuildBankData().money == 5326107u);
}
