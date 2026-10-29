// The auction house, end to end: the auctioneer's packets, what the panel
// reads and is told, the lines the player sees, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "auction/domain/records.hpp"
#include "auction/services/auction_house.hpp"
#include "game/game_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kAuctioneer = 0xF130000000000AAAull;
constexpr uint64_t kSeller = 0x41;
constexpr uint32_t kLinen = 2589;

/// MSG_AUCTION_HELLO: the auctioneer, the house, and whether it is open.
std::vector<uint8_t> hello() {
    return Body().u64(kAuctioneer).u32(7).u8(1).bytes();
}

/// An auction list result as 3.3.5a writes it: each row's id and item, seven
/// enchant triplets, the random property, the stack and the prices, then the
/// total and the server's search delay in milliseconds.
std::vector<uint8_t> listResult(const std::vector<std::pair<uint32_t, uint32_t>>& rows,
                                uint32_t searchDelayMs = 300) {
    Body b;
    b.u32(static_cast<uint32_t>(rows.size()));
    for (const auto& [auctionId, item] : rows) {
        b.u32(auctionId).u32(item);
        for (int i = 0; i < 21; ++i) b.u32(0);         // seven enchant slots
        b.u32(0).u32(0).u32(20).u32(0).u32(0);         // property, suffix, stack, charges, flags
        b.u64(kSeller).u32(100).u32(5).u32(900).u32(3600000).u64(0).u32(0);
    }
    b.u32(static_cast<uint32_t>(rows.size())).u32(searchDelayMs);
    return b.bytes();
}

std::vector<uint8_t> commandResult(uint32_t action, uint32_t error) {
    return Body().u32(42).u32(action).u32(error).bytes();
}

void search(GameHarness& h, const std::string& name) {
    h.auctionHouse().auctionSearch(name, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0);
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

TEST_CASE("the auctioneer's greeting opens the house and the panel is told", "[auction][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();

    REQUIRE(h.auctionHouse().isAuctionHouseOpen());
    CHECK(h.auctionHouse().getAuctioneerGuid() == kAuctioneer);
    CHECK(h.addonEvents("AUCTION_HOUSE_SHOW").size() == 1);

    h.auctionHouse().closeAuctionHouse();
    h.pump();
    CHECK_FALSE(h.auctionHouse().isAuctionHouseOpen());
    CHECK(h.addonEvents("AUCTION_HOUSE_CLOSED").size() == 1);
}

TEST_CASE("a search goes to the auctioneer, and its answer fills the browse list", "[auction][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();
    h.clearRecords();

    search(h, "Linen");
    auto* request = h.lastSent(Opcode::CMSG_AUCTION_LIST_ITEMS);
    REQUIRE(request);
    CHECK(request->readUInt64() == kAuctioneer);
    request->readUInt32();                                  // offset
    CHECK(request->readString() == "Linen");
    // The button waits until the answer says how long.
    CHECK(h.auctionHouse().getAuctionSearchDelay() == Catch::Approx(5.0f));

    h.receive(Opcode::SMSG_AUCTION_LIST_RESULT, listResult({{501, kLinen}}));
    h.pump();

    const auto& rows = h.auctionHouse().getAuctionBrowseResults().auctions;
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].auctionId == 501);
    CHECK(rows[0].stackCount == 20);
    CHECK(rows[0].buyoutPrice == 900);
    CHECK(h.addonEvents("AUCTION_ITEM_LIST_UPDATE").size() == 1);
    // The server's 300 ms, less the frame that handled it.
    CHECK(h.auctionHouse().getAuctionSearchDelay() <= 0.3f);
    CHECK(h.auctionHouse().getAuctionSearchDelay() > 0.25f);
    // What the row needs to be drawn: the item and the seller's name.
    auto* itemQuery = h.lastSent(Opcode::CMSG_ITEM_QUERY_SINGLE);
    REQUIRE(itemQuery);
    CHECK(itemQuery->readUInt32() == kLinen);
    CHECK(h.lastSent(Opcode::CMSG_NAME_QUERY));

    h.advance(0.5f);
    CHECK(h.auctionHouse().getAuctionSearchDelay() == 0.0f);
}

TEST_CASE("the player's own auctions go to the owned list, as does a plain list after asking",
          "[auction][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();

    h.auctionHouse().auctionListOwnerItems();
    CHECK(h.lastSent(Opcode::CMSG_AUCTION_LIST_OWNER_ITEMS));
    // A search went out since; the owner result still says which list it is.
    search(h, "Linen");
    h.receive(Opcode::SMSG_AUCTION_OWNER_LIST_RESULT, listResult({{601, kLinen}, {602, kLinen}}));
    h.pump();
    CHECK(h.auctionHouse().getAuctionOwnerResults().auctions.size() == 2);
    CHECK(h.auctionHouse().getAuctionBrowseResults().auctions.empty());
    CHECK(h.addonEvents("AUCTION_OWNED_LIST_UPDATE").size() == 1);

    // The bids, asked for, arriving on the plain opcode.
    h.auctionHouse().auctionListBidderItems();
    h.receive(Opcode::SMSG_AUCTION_LIST_RESULT, listResult({{701, kLinen}}));
    h.pump();
    CHECK(h.auctionHouse().getAuctionBidderResults().auctions.size() == 1);
    CHECK(h.auctionHouse().getAuctionBrowseResults().auctions.empty());
    CHECK(h.addonEvents("AUCTION_BIDDER_LIST_UPDATE").size() == 1);
}

TEST_CASE("a won bid is said, the bags and coin redrawn, and the search asked again",
          "[auction][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();
    search(h, "Linen");
    h.receive(Opcode::SMSG_AUCTION_LIST_RESULT, listResult({{501, kLinen}}));
    h.pump();
    h.clearRecords();

    h.auctionHouse().auctionBuyout(501, 900);
    auto* bid = h.lastSent(Opcode::CMSG_AUCTION_PLACE_BID);
    REQUIRE(bid);
    CHECK(bid->readUInt64() == kAuctioneer);
    CHECK(bid->readUInt32() == 501);
    CHECK(bid->readUInt32() == 900);

    h.receive(Opcode::SMSG_AUCTION_COMMAND_RESULT, commandResult(2, 0));
    h.pump();
    CHECK(chatSaid(h, "Auction bid/buyout successful."));
    CHECK(h.addonEvents("PLAYER_MONEY").size() == 1);
    CHECK_FALSE(h.addonEvents("BAG_UPDATE").empty());
    auto* again = h.lastSent(Opcode::CMSG_AUCTION_LIST_ITEMS);
    REQUIRE(again);
    again->readUInt64();
    again->readUInt32();
    CHECK(again->readString() == "Linen");
}

TEST_CASE("a sale that went through does not search again", "[auction][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();
    search(h, "Linen");
    h.clearRecords();

    h.receive(Opcode::SMSG_AUCTION_COMMAND_RESULT, commandResult(0, 0));
    h.pump();

    CHECK(chatSaid(h, "Auction sell successful."));
    CHECK_FALSE(h.lastSent(Opcode::CMSG_AUCTION_LIST_ITEMS));
}

TEST_CASE("a refused command says why on the error line and in chat", "[auction][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();
    search(h, "Linen");
    h.clearRecords();

    h.receive(Opcode::SMSG_AUCTION_COMMAND_RESULT, commandResult(2, 5));
    h.receive(Opcode::SMSG_AUCTION_COMMAND_RESULT, commandResult(1, 99));
    h.pump();

    CHECK(uiErrors(h) == std::vector<std::string>{"Auction bid/buyout failed: Bid too low.",
                                                  "Auction cancel failed: Unknown error."});
    CHECK(chatSaid(h, "Auction bid/buyout failed: Bid too low."));
    CHECK_FALSE(h.lastSent(Opcode::CMSG_AUCTION_LIST_ITEMS));
}

TEST_CASE("an item's description arriving redraws the open list waiting on it", "[auction][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();
    search(h, "Linen");
    h.receive(Opcode::SMSG_AUCTION_LIST_RESULT, listResult({{501, kLinen}}));
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, GameHarness::itemTemplateBody(kLinen, "Linen Cloth", 7, 0));
    h.pump();
    CHECK(h.addonEvents("AUCTION_ITEM_LIST_UPDATE").size() == 1);
    CHECK(h.addonEvents("AUCTION_OWNED_LIST_UPDATE").empty());

    // Shut, the panel has nothing to redraw.
    h.auctionHouse().closeAuctionHouse();
    h.clearRecords();
    h.receive(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, GameHarness::itemTemplateBody(kLinen, "Linen Cloth", 7, 0));
    h.pump();
    CHECK(h.addonEvents("AUCTION_ITEM_LIST_UPDATE").empty());
}

TEST_CASE("nothing goes out away from an auctioneer", "[auction][harness]") {
    GameHarness h;
    h.enterWorld();

    search(h, "Linen");
    h.auctionHouse().auctionPlaceBid(501, 100);
    h.auctionHouse().auctionListOwnerItems();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_AUCTION_LIST_ITEMS));
    CHECK_FALSE(h.lastSent(Opcode::CMSG_AUCTION_PLACE_BID));
    CHECK_FALSE(h.lastSent(Opcode::CMSG_AUCTION_LIST_OWNER_ITEMS));
}

TEST_CASE("the panel's column sort reorders the list it is given", "[auction][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();
    search(h, "");
    h.receive(Opcode::SMSG_AUCTION_LIST_RESULT, listResult({{503, kLinen}, {501, kLinen}, {502, kLinen}}));
    h.pump();

    h.auctionHouse().reorderAuctionList(wowee::auction::AuctionList::Browse,
                                        [](const auto& a, const auto& b) { return a.auctionId < b.auctionId; });

    const auto& rows = h.auctionHouse().getAuctionBrowseResults().auctions;
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].auctionId == 501);
    CHECK(rows[2].auctionId == 503);
}

TEST_CASE("the next character is at no auctioneer", "[auction][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::MSG_AUCTION_HELLO, hello());
    h.pump();
    search(h, "Linen");
    h.receive(Opcode::SMSG_AUCTION_LIST_RESULT, listResult({{501, kLinen}}));
    h.pump();
    REQUIRE(h.auctionHouse().isAuctionHouseOpen());

    h.relogAs(bob);

    CHECK_FALSE(h.auctionHouse().isAuctionHouseOpen());
    CHECK(h.auctionHouse().getAuctionBrowseResults().auctions.empty());
    search(h, "Linen");
    CHECK_FALSE(h.lastSent(Opcode::CMSG_AUCTION_LIST_ITEMS));
}
