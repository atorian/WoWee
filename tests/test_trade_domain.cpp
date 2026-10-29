// Trading, end to end: the server's trade status packets, what the trade
// frame reads and is told, the lines the player sees, and the requests that
// go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "trade/domain/records.hpp"
#include "trade/services/trade.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kJaina = 0x21;
constexpr uint32_t kLinen = 2589;

/// SMSG_TRADE_STATUS with AzerothCore's code; a request carries who asks.
std::vector<uint8_t> status(uint32_t code, uint64_t peer = 0) {
    Body b;
    b.u32(code);
    if (code == 1) b.u64(peer);
    return b.bytes();
}

/// SMSG_TRADE_STATUS_EXTENDED: one side's offer, with one item in slot 0 and
/// six empty slots after it, each seventy-three bytes.
std::vector<uint8_t> offer(bool own, uint32_t gold, uint32_t item, uint32_t stack) {
    Body b;
    b.u8(own ? 0 : 1).u32(1).u32(7).u32(7).u32(gold).u32(0);
    for (uint8_t slot = 0; slot < 7; ++slot) {
        const bool filled = slot == 0;
        b.u8(slot).u32(filled ? item : 0).u32(filled ? 77 : 0).u32(filled ? stack : 0).u32(0);
        b.u64(0).u32(0).u32(0).u32(0).u32(0).u64(0);
        b.u32(0).u32(0).u32(0).u32(0).u32(0).u32(0);
    }
    return b.bytes();
}

void knowJaina(GameHarness& h) {
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE,
              Body().packedGuid(kJaina).u8(0).cstr("Jaina").cstr("").u8(1).u8(1).u8(8).bytes());
    h.pump();
}

/// Jaina asked and the window is open.
void trading(GameHarness& h) {
    knowJaina(h);
    h.receive(Opcode::SMSG_TRADE_STATUS, status(1, kJaina));
    h.receive(Opcode::SMSG_TRADE_STATUS, status(2));
    h.pump();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

std::vector<std::string> uiErrors(GameHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.addonEvents("UI_ERROR_MESSAGE")) out.push_back(e.args.at(0));
    return out;
}

}  // namespace

TEST_CASE("a request names who asks, and answering it begins the trade", "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    knowJaina(h);

    h.receive(Opcode::SMSG_TRADE_STATUS, status(1, kJaina));
    h.pump();

    CHECK(chatSaid(h, "Jaina wants to trade with you."));
    const auto asked = h.addonEvents("TRADE_REQUEST");
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].args == std::vector<std::string>{"Jaina"});
    CHECK_FALSE(h.trade().isTradeOpen());

    h.trade().acceptTradeRequest();
    CHECK(h.lastSent(Opcode::CMSG_BEGIN_TRADE));

    h.receive(Opcode::SMSG_TRADE_STATUS, status(2));
    h.pump();
    CHECK(h.trade().isTradeOpen());
    CHECK(h.trade().getTradePeerGuid() == kJaina);
    CHECK(h.addonEvents("TRADE_SHOW").size() == 1);
    // The trade partner is the unit the frame calls "NPC".
    CHECK(h.game().getInteractNpcGuid() == kJaina);
}

TEST_CASE("a request from someone not yet named is from Unknown", "[trade][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_TRADE_STATUS, status(1, 0x99));
    h.pump();

    CHECK(chatSaid(h, "Unknown wants to trade with you."));
}

TEST_CASE("with trades blocked a request is turned down before any window", "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    knowJaina(h);
    h.cvars().set("blockTrades", "1");

    h.receive(Opcode::SMSG_TRADE_STATUS, status(1, kJaina));
    h.pump();

    CHECK(chatSaid(h, "Declined a trade from Jaina (trades are blocked)."));
    CHECK(h.lastSent(Opcode::CMSG_CANCEL_TRADE));
    CHECK(h.addonEvents("TRADE_REQUEST").empty());
    h.trade().acceptTradeRequest();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_BEGIN_TRADE));
}

TEST_CASE("each side's offer fills its own slots, and each money frame hears only its own side",
          "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    trading(h);
    h.clearRecords();

    h.receive(Opcode::SMSG_TRADE_STATUS_EXTENDED, offer(false, 500, kLinen, 20));
    h.pump();

    const auto& theirs = h.trade().getPeerTradeSlots();
    CHECK(theirs[0].itemId == kLinen);
    CHECK(theirs[0].stackCount == 20);
    CHECK(h.trade().getMyTradeSlots()[0].itemId == 0);
    CHECK(h.trade().getPeerTradeGold() == 500);
    CHECK(h.addonEvents("TRADE_UPDATE").size() == 1);
    CHECK(h.addonEvents("TRADE_MONEY_CHANGED").size() == 1);
    CHECK(h.addonEvents("PLAYER_TRADE_MONEY").empty());
    // The item is asked about so the slot can name it.
    auto* query = h.lastSent(Opcode::CMSG_ITEM_QUERY_SINGLE);
    REQUIRE(query);
    CHECK(query->readUInt32() == kLinen);

    // The same gold again: the slots are redrawn, the money is not.
    h.receive(Opcode::SMSG_TRADE_STATUS_EXTENDED, offer(false, 500, kLinen, 20));
    h.receive(Opcode::SMSG_TRADE_STATUS_EXTENDED, offer(true, 30, 0, 0));
    h.pump();
    CHECK(h.addonEvents("TRADE_UPDATE").size() == 3);
    CHECK(h.addonEvents("TRADE_MONEY_CHANGED").size() == 1);
    CHECK(h.addonEvents("PLAYER_TRADE_MONEY").size() == 1);
    CHECK(h.trade().getMyTradeGold() == 30);
}

TEST_CASE("whoever accepts second can still accept, and the frame hears both sides", "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    trading(h);
    h.clearRecords();

    h.receive(Opcode::SMSG_TRADE_STATUS, status(4 /* partner accepted */));
    h.pump();
    CHECK(chatSaid(h, "Trade partner accepted."));
    auto updates = h.addonEvents("TRADE_ACCEPT_UPDATE");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].args == std::vector<std::string>{"0", "1"});

    h.trade().acceptTrade();
    h.pump();
    CHECK(h.lastSent(Opcode::CMSG_ACCEPT_TRADE));
    updates = h.addonEvents("TRADE_ACCEPT_UPDATE");
    REQUIRE(updates.size() == 2);
    CHECK(updates[1].args == std::vector<std::string>{"1", "1"});

    // Once accepted, nothing on the table changes.
    h.trade().setTradeGold(100);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_SET_TRADE_GOLD));
}

TEST_CASE("only one's own acceptance is taken back, and back-to-trade clears both", "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    trading(h);
    h.receive(Opcode::SMSG_TRADE_STATUS, status(4));
    h.pump();
    h.clearRecords();

    // The partner's acceptance is not ours to take back.
    h.trade().unacceptTrade();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_UNACCEPT_TRADE));

    h.receive(Opcode::SMSG_TRADE_STATUS, status(7 /* back to trade */));
    h.pump();
    const auto updates = h.addonEvents("TRADE_ACCEPT_UPDATE");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].args == std::vector<std::string>{"0", "0"});
    h.trade().setTradeGold(100);
    auto* gold = h.lastSent(Opcode::CMSG_SET_TRADE_GOLD);
    REQUIRE(gold);
    CHECK(gold->readUInt32() == 100);
}

TEST_CASE("a completed trade closes the window, redraws bags and coin, and keeps what was traded",
          "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    trading(h);
    h.receive(Opcode::SMSG_TRADE_STATUS_EXTENDED, offer(false, 500, kLinen, 20));
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_TRADE_STATUS, status(8 /* complete */));
    h.pump();

    CHECK(chatSaid(h, "Trade complete."));
    CHECK(h.addonEvents("TRADE_CLOSED").size() == 1);
    CHECK_FALSE(h.addonEvents("BAG_UPDATE").empty());
    CHECK(h.addonEvents("PLAYER_MONEY").size() == 1);
    CHECK_FALSE(h.trade().isTradeOpen());
    // The final offer may still be on its way; what was traded stays readable.
    CHECK(h.trade().getPeerTradeSlots()[0].itemId == kLinen);
    CHECK(h.trade().getPeerTradeGold() == 500);
}

TEST_CASE("a cancelled trade forgets everything", "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    trading(h);
    h.receive(Opcode::SMSG_TRADE_STATUS_EXTENDED, offer(false, 500, kLinen, 20));
    h.pump();

    h.receive(Opcode::SMSG_TRADE_STATUS, status(3 /* cancelled */));
    h.pump();

    CHECK(chatSaid(h, "Trade cancelled."));
    CHECK(h.addonEvents("TRADE_CLOSED").size() == 1);
    CHECK(h.trade().getPeerTradeSlots()[0].itemId == 0);
    CHECK(h.trade().getPeerTradeGold() == 0);
    CHECK(h.trade().getTradePeerGuid() == 0);
}

TEST_CASE("a refusal ends the trade or not, and is said in chat or on the error line as it always was",
          "[trade][harness]") {
    GameHarness h;
    h.enterWorld();
    trading(h);

    // Stunned: said, and the trade goes on.
    h.receive(Opcode::SMSG_TRADE_STATUS, status(15));
    h.pump();
    CHECK(h.trade().isTradeOpen());

    // Too far: said in chat, and it is over.
    h.receive(Opcode::SMSG_TRADE_STATUS, status(10));
    h.pump();
    CHECK_FALSE(h.trade().isTradeOpen());
    CHECK(chatSaid(h, "Trade failed: target is too far away."));

    h.receive(Opcode::SMSG_TRADE_STATUS, status(11));
    h.pump();
    CHECK(uiErrors(h) == std::vector<std::string>{"You are stunned.", "You cannot trade with the enemy."});
}

TEST_CASE("asking to trade needs someone to trade with", "[trade][harness]") {
    GameHarness h;
    h.enterWorld();

    h.trade().initiateTrade(0);
    h.pump();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_INITIATE_TRADE));
    CHECK(uiErrors(h) == std::vector<std::string>{"You must target a player to trade with."});

    h.trade().initiateTrade(kJaina);
    auto* ask = h.lastSent(Opcode::CMSG_INITIATE_TRADE);
    REQUIRE(ask);
    CHECK(ask->readUInt64() == kJaina);
    CHECK(chatSaid(h, "Requesting trade with target."));
}

TEST_CASE("the next character has none of the last one's trade", "[trade][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    trading(h);
    h.receive(Opcode::SMSG_TRADE_STATUS_EXTENDED, offer(false, 500, kLinen, 20));
    h.pump();
    REQUIRE(h.trade().isTradeOpen());

    h.relogAs(bob);

    CHECK_FALSE(h.trade().isTradeOpen());
    CHECK(h.trade().getTradePeerGuid() == 0);
    CHECK(h.trade().getPeerTradeGold() == 0);
    h.trade().acceptTrade();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_ACCEPT_TRADE));
}
