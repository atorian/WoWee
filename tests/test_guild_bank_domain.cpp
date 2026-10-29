// The guild vault, end to end: the banker's packets, what the bank frame
// reads and is told, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "guild_bank/domain/records.hpp"
#include "guild_bank/services/guild_bank.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kBanker = 0xF11000000000BA2Cull;
constexpr uint32_t kLinen = 2589;

/// The player is in a guild: SMSG_GUILD_ROSTER, sent only to a member, here
/// with no one on it.
void inAGuild(GameHarness& h) {
    h.receive(Opcode::SMSG_GUILD_ROSTER, Body().u32(0).cstr("").cstr("").u32(0).bytes());
    h.pump();
}

/// SMSG_GUILD_BANK_LIST: the money, the tab it describes, the withdrawal
/// allowance, the tab names when `tabNames` is given, then the tab's slots,
/// each a plain item with no property or sockets.
std::vector<uint8_t> bankList(uint8_t tab, uint64_t money, const std::vector<std::string>& tabNames,
                              const std::vector<uint8_t>& occupiedSlots) {
    Body b;
    b.u64(money).u8(tab).u32(250).u8(tabNames.empty() ? 0 : 1);
    if (!tabNames.empty()) {
        b.u8(static_cast<uint8_t>(tabNames.size()));
        for (const auto& name : tabNames) b.cstr(name).cstr("INV_Misc_Bag_01");
    }
    b.u8(static_cast<uint8_t>(occupiedSlots.size()));
    for (uint8_t slot : occupiedSlots) {
        b.u8(slot).u32(kLinen).u32(0).u32(0);       // slot, item, flags, no property
        b.u32(20).u32(0).u8(0).u8(0);               // stack, enchant, charges, no sockets
    }
    return b.bytes();
}

void atVault(GameHarness& h, const std::vector<uint8_t>& occupied = {}) {
    inAGuild(h);
    h.guildBank().openGuildBank(kBanker);
    h.receive(Opcode::SMSG_GUILD_BANK_LIST, bankList(0, 5000, {"Mats", "Gear"}, occupied));
    h.pump();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

}  // namespace

TEST_CASE("a player in no guild is told so, and the banker is not asked", "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();

    h.guildBank().openGuildBank(kBanker);
    h.pump();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_BANKER_ACTIVATE));
    const auto errors = h.addonEvents("UI_ERROR_MESSAGE");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].args.at(0) == "You are not in a guild.");
}

TEST_CASE("the vault opens when its list arrives, not when it is asked", "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();
    inAGuild(h);
    h.clearRecords();

    h.guildBank().openGuildBank(kBanker);
    h.pump();
    auto* activate = h.lastSent(Opcode::CMSG_GUILD_BANKER_ACTIVATE);
    REQUIRE(activate);
    CHECK(activate->readUInt64() == kBanker);
    auto* query = h.lastSent(Opcode::CMSG_GUILD_BANK_QUERY_TAB);
    REQUIRE(query);
    CHECK(query->readUInt64() == kBanker);
    CHECK(query->readUInt8() == 0);
    CHECK_FALSE(h.guildBank().isGuildBankOpen());
    CHECK(h.addonEvents("GUILDBANKFRAME_OPENED").empty());

    h.receive(Opcode::SMSG_GUILD_BANK_LIST, bankList(0, 5000, {"Mats", "Gear"}, {0}));
    h.pump();

    REQUIRE(h.guildBank().isGuildBankOpen());
    const auto& data = h.guildBank().getGuildBankData();
    CHECK(data.money == 5000);
    CHECK(data.withdrawAmount == 250);
    REQUIRE(data.tabs.size() == 2);
    CHECK(data.tabs[1].tabName == "Gear");
    REQUIRE(data.tabItems.size() == 1);
    CHECK(data.tabItems[0].itemEntry == kLinen);
    // The window, the tabs, the slots, the money and the allowance, in that order.
    const size_t opened = h.firedAt("GUILDBANKFRAME_OPENED");
    const size_t tabs = h.firedAt("GUILDBANK_UPDATE_TABS");
    const size_t slots = h.firedAt("GUILDBANKBAGSLOTS_CHANGED");
    const size_t money = h.firedAt("GUILDBANK_UPDATE_MONEY");
    const size_t allowance = h.firedAt("GUILDBANK_UPDATE_WITHDRAWMONEY");
    REQUIRE(allowance < h.addonEvents().size());
    CHECK(opened < tabs);
    CHECK(tabs < slots);
    CHECK(slots < money);
    CHECK(money < allowance);
}

TEST_CASE("a list of one tab's slots keeps the tab names, and that tab is the one viewed",
          "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();
    atVault(h);
    h.clearRecords();

    h.receive(Opcode::SMSG_GUILD_BANK_LIST, bankList(1, 5000, {}, {3}));
    h.pump();

    const auto& data = h.guildBank().getGuildBankData();
    REQUIRE(data.tabs.size() == 2);
    CHECK(data.tabs[0].tabName == "Mats");
    CHECK(h.guildBank().getGuildBankActiveTab() == 1);
    CHECK(h.addonEvents("GUILDBANK_UPDATE_TABS").empty());
    CHECK(h.addonEvents("GUILDBANKBAGSLOTS_CHANGED").size() == 1);
}

TEST_CASE("a deposit from the bags goes to the first empty slot of the tab viewed", "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();
    atVault(h, {0, 1});
    h.clearRecords();

    h.guildBank().guildBankDepositFromInventory(0xFF, 23);

    auto* swap = h.lastSent(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS);
    REQUIRE(swap);
    CHECK(swap->readUInt64() == kBanker);
    swap->readUInt8();                                  // not bank to bank
    CHECK(swap->readUInt8() == 0);                      // the tab
    CHECK(swap->readUInt8() == 2);                      // the first empty slot
}

TEST_CASE("a deposit into a full tab is said and not sent", "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();
    std::vector<uint8_t> all;
    for (uint8_t s = 0; s < 98; ++s) all.push_back(s);
    atVault(h, all);
    h.clearRecords();

    h.guildBank().guildBankDepositFromInventory(0xFF, 23);
    h.pump();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS));
    CHECK(chatSaid(h, "This guild bank tab is full."));
}

TEST_CASE("a tab's text is kept and the frame told which tab, counting from one", "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();
    atVault(h);

    h.guildBank().queryGuildBankText(1);
    auto* ask = h.lastSent(Opcode::MSG_QUERY_GUILD_BANK_TEXT);
    REQUIRE(ask);
    CHECK(ask->readUInt8() == 1);

    h.receive(Opcode::MSG_QUERY_GUILD_BANK_TEXT, Body().u8(1).cstr("Raid gear only").bytes());
    h.pump();
    CHECK(h.guildBank().getGuildBankTabText(1) == "Raid gear only");
    const auto told = h.addonEvents("GUILDBANK_UPDATE_TEXT");
    REQUIRE(told.size() == 1);
    CHECK(told[0].args == std::vector<std::string>{"2"});
}

TEST_CASE("a tab's log is read by the entry types it carries, and a cut-short one leaves it",
          "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();
    atVault(h);

    h.guildBank().requestGuildBankLog(0);
    auto* ask = h.lastSent(Opcode::MSG_GUILD_BANK_LOG_QUERY);
    REQUIRE(ask);
    CHECK(ask->readUInt8() == 0);

    // Tab 0: a deposit of an item (type 1) and a move to tab 2 (type 3).
    h.receive(Opcode::MSG_GUILD_BANK_LOG_QUERY,
              Body().u8(0).u8(2)
                  .u8(1).u64(0x21).u32(kLinen).u32(20).u32(60)
                  .u8(3).u64(0x22).u32(kLinen).u32(5).u8(2).u32(30)
                  .bytes());
    h.pump();
    const auto& log = h.guildBank().getGuildBankLog(0);
    REQUIRE(log.size() == 2);
    CHECK(log[0].itemId == kLinen);
    CHECK(log[0].count == 20);
    CHECK(log[1].otherTab == 2);
    CHECK(log[1].secondsAgo == 30);
    CHECK(h.addonEvents("GUILDBANKLOG_UPDATE").size() == 1);

    // An item entry that ends inside its item: the log shown stays.
    h.receive(Opcode::MSG_GUILD_BANK_LOG_QUERY, Body().u8(0).u8(1).u8(1).u64(0x21).u32(kLinen).bytes());
    h.pump();
    CHECK(h.guildBank().getGuildBankLog(0).size() == 2);
    CHECK(h.addonEvents("GUILDBANKLOG_UPDATE").size() == 1);
}

TEST_CASE("closing the vault tells the frame and asks nothing more", "[guild_bank][harness]") {
    GameHarness h;
    h.enterWorld();
    atVault(h);

    h.guildBank().closeGuildBank();
    h.pump();
    CHECK_FALSE(h.guildBank().isGuildBankOpen());
    CHECK(h.addonEvents("GUILDBANKFRAME_CLOSED").size() == 1);

    h.clearRecords();
    h.guildBank().queryGuildBankTab(1);
    h.guildBank().depositGuildBankMoney(100);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_BANK_QUERY_TAB));
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_BANK_DEPOSIT_MONEY));
}

TEST_CASE("the next character is at no banker and has none of the last one's vault", "[guild_bank][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    atVault(h);
    REQUIRE(h.guildBank().isGuildBankOpen());

    h.relogAs(bob);

    CHECK_FALSE(h.guildBank().isGuildBankOpen());
    CHECK(h.guildBank().getGuildBankData().tabs.empty());
    CHECK(h.guildBank().getGuildBankData().money == 0);
    h.guildBank().queryGuildBankTab(0);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_BANK_QUERY_TAB));
}
