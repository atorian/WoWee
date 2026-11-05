// The guild vault: its domain model, and the service against a recording
// gateway and the real event bus. Only wowee_guild_bank is linked.
#include <catch_amalgamated.hpp>

#include "foundation/event_bus.hpp"
#include "guild_bank/services/guild_bank_service.hpp"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::guild_bank;

namespace {

template <typename E>
bool has(const std::vector<VaultChange>& changes) {
    return std::any_of(changes.begin(), changes.end(),
                       [](const VaultChange& c) { return std::holds_alternative<E>(c); });
}

GuildBankItemSlot item(uint8_t slot, uint32_t entry) {
    GuildBankItemSlot s;
    s.slotId = slot;
    s.itemEntry = entry;
    return s;
}

GuildBankData list(uint8_t tabId, std::vector<GuildBankItemSlot> items = {}) {
    GuildBankData d;
    d.tabId = tabId;
    d.tabItems = std::move(items);
    return d;
}

GuildBankData withTabs(int count) {
    GuildBankData d;
    d.tabsIncluded = true;
    d.tabs.resize(static_cast<size_t>(count));
    return d;
}

struct RecordingGateway final : IGuildBankGateway, IGuildBankWorld {
    bool world = true, guild = true;
    std::vector<std::string> sent;
    std::vector<uint32_t> asked;

    static std::string n(unsigned v) { return std::to_string(v); }

    bool online() const override { return world; }
    void sendActivate(uint64_t banker) override { sent.push_back("activate " + n(banker)); }
    void sendQueryTab(uint64_t, uint8_t tab) override { sent.push_back("query " + n(tab)); }
    void sendBuyTab(uint64_t, uint8_t tab) override { sent.push_back("buy " + n(tab)); }
    void sendDepositMoney(uint64_t, uint32_t amount) override { sent.push_back("deposit money " + n(amount)); }
    void sendWithdrawMoney(uint64_t, uint32_t amount) override { sent.push_back("withdraw money " + n(amount)); }
    void sendWithdrawItem(uint64_t, uint8_t tab, uint8_t slot, uint8_t, uint8_t, uint32_t) override {
        sent.push_back("withdraw " + n(tab) + ":" + n(slot));
    }
    void sendDepositItem(uint64_t, uint8_t tab, uint8_t slot, uint8_t, uint8_t) override {
        sent.push_back("deposit " + n(tab) + ":" + n(slot));
    }
    void sendQueryText(uint8_t tab) override { sent.push_back("text? " + n(tab)); }
    void sendTabText(uint8_t tab, const std::string& text) override { sent.push_back("text " + n(tab) + " " + text); }
    void sendUpdateTab(uint64_t, uint8_t tab, const std::string& name, const std::string&) override {
        sent.push_back("rename " + n(tab) + " " + name);
    }
    void sendLogQuery(uint8_t tab) override { sent.push_back("log? " + n(tab)); }

    bool inGuild() const override { return guild; }
    void requestItem(uint32_t entry) override { asked.push_back(entry); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    GuildBankService service{gateway, gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<OpenRefused>([this](const auto&) { events.push_back("refused"); }));
        subscriptions.push_back(bus.subscribe<TabFull>([this](const auto&) { events.push_back("full"); }));
        subscriptions.push_back(bus.subscribe<VaultOpened>([this](const auto&) { events.push_back("opened"); }));
        subscriptions.push_back(bus.subscribe<VaultClosed>([this](const auto&) { events.push_back("closed"); }));
    }

    /// At banker 9, with the tab list in: the window is up.
    void atVault(GuildBankData contents = {}) {
        service.openGuildBank(9);
        service.onBankList(std::move(contents));
        gateway.sent.clear();
        events.clear();
    }
};

} // namespace

TEST_CASE("The vault opens when its list arrives, not when it is asked", "[guild_bank]") {
    GuildBank bank;
    // A list nobody asked for opens nothing: there is no banker to go with it.
    CHECK_FALSE(has<VaultOpened>(bank.contentsReceived(list(0))));
    CHECK_FALSE(bank.isOpen());
    bank.activationSent(9);
    CHECK_FALSE(bank.isOpen());
    CHECK(has<VaultOpened>(bank.contentsReceived(list(0))));
    CHECK(bank.isOpen());
    CHECK_FALSE(has<VaultOpened>(bank.contentsReceived(list(1))));
}

TEST_CASE("Tabs are announced only when a list names them", "[guild_bank]") {
    GuildBank bank;
    CHECK(has<TabsChanged>(bank.contentsReceived(withTabs(2))));
    auto slotsOnly = bank.contentsReceived(list(0));
    CHECK_FALSE(has<TabsChanged>(slotsOnly));
    CHECK(has<SlotsChanged>(slotsOnly));
    CHECK_FALSE(has<TabsChanged>(bank.contentsReceived(withTabs(0))));
}

TEST_CASE("The tab being viewed is the one the last list describes", "[guild_bank]") {
    GuildBank bank;
    bank.activationSent(9);
    bank.contentsReceived(list(3));
    CHECK(bank.activeTab() == 3);
}

TEST_CASE("The balance a guild event reports is announced only when it moves", "[guild_bank]") {
    GuildBank bank;
    CHECK(has<MoneyChanged>(bank.moneyReported(500)));
    CHECK(bank.contents().money == 500);
    CHECK(bank.moneyReported(500).empty());
}

TEST_CASE("A deposit from the bags takes the first empty slot of the tab", "[guild_bank]") {
    GuildBank bank;
    bank.contentsReceived(list(0, {item(0, 1), item(1, 0), item(2, 5)}));
    CHECK(bank.freeSlot() == uint8_t{1});
    bank.contentsReceived(list(0, {item(1, 1), item(3, 5)}));
    CHECK(bank.freeSlot() == uint8_t{0});

    GuildBankData full = list(0);
    for (int s = 0; s < kTabSlots; ++s) full.tabItems.push_back(item(static_cast<uint8_t>(s), 7));
    bank.contentsReceived(full);
    CHECK_FALSE(bank.freeSlot());
}

TEST_CASE("A tab's text is kept and announced counting from one", "[guild_bank]") {
    GuildBank bank;
    auto changes = bank.tabTextReceived(2, "Herbs only");
    REQUIRE(changes.size() == 1);
    CHECK(std::get<TabTextChanged>(changes[0]).tab == 3);
    CHECK(bank.tabText(2) == "Herbs only");
    CHECK(bank.tabText(kItemTabs).empty());
}

TEST_CASE("The money log is the seventh, and nothing lies past it", "[guild_bank]") {
    GuildBank bank;
    GuildBankLogEntry e;
    e.count = 100;
    CHECK(has<LogChanged>(bank.logReceived(kMoneyLogTab, {e})));
    CHECK(bank.log(kMoneyLogTab).size() == 1);
    CHECK(bank.logReceived(kMoneyLogTab + 1, {e}).empty());
    CHECK(bank.log(kMoneyLogTab + 1).empty());
}

TEST_CASE("A player in no guild is told so, and nothing is asked", "[guild_bank]") {
    Fixture f;
    f.gateway.guild = false;
    f.service.openGuildBank(9);
    CHECK(f.events == std::vector<std::string>{"refused"});
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("Opening registers with the banker, then asks for the first tab", "[guild_bank]") {
    Fixture f;
    f.service.openGuildBank(9);
    CHECK(f.gateway.sent == std::vector<std::string>{"activate 9", "query 0"});
    CHECK(f.events.empty());
}

TEST_CASE("After closing, the vault asks nothing more of the banker", "[guild_bank]") {
    Fixture f;
    f.atVault();
    f.service.closeGuildBank();
    CHECK_FALSE(f.service.isGuildBankOpen());
    f.service.queryGuildBankTab(1);
    f.service.depositGuildBankMoney(10);
    CHECK(f.gateway.sent.empty());
    CHECK(f.events == std::vector<std::string>{"closed"});
}

TEST_CASE("Nothing is sent while offline", "[guild_bank]") {
    Fixture f;
    f.atVault();
    f.gateway.world = false;
    f.service.withdrawGuildBankMoney(10);
    f.service.queryGuildBankText(0);
    f.service.requestGuildBankLog(0);
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("A bag deposit goes to the tab being viewed, or says it is full", "[guild_bank]") {
    Fixture f;
    f.atVault(list(2, {item(0, 1)}));
    f.service.guildBankDepositFromInventory(0, 5);
    CHECK(f.gateway.sent == std::vector<std::string>{"deposit 2:1"});

    GuildBankData full = list(2);
    for (int s = 0; s < kTabSlots; ++s) full.tabItems.push_back(item(static_cast<uint8_t>(s), 7));
    f.service.onBankList(full);
    f.gateway.sent.clear();
    f.service.guildBankDepositFromInventory(0, 5);
    CHECK(f.gateway.sent.empty());
    CHECK(f.events == std::vector<std::string>{"full"});
}

TEST_CASE("A bag deposit waits for the window", "[guild_bank]") {
    Fixture f;
    f.service.openGuildBank(9);
    f.gateway.sent.clear();
    f.service.guildBankDepositFromInventory(0, 5);
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("Buying a tab buys the next one", "[guild_bank]") {
    Fixture f;
    f.atVault(withTabs(3));
    f.service.buyGuildBankTab();
    CHECK(f.gateway.sent == std::vector<std::string>{"buy 3"});
}

TEST_CASE("Renaming a tab needs both a name and an icon", "[guild_bank]") {
    Fixture f;
    f.atVault();
    f.service.setGuildBankTabInfo(1, "Ore", "");
    f.service.setGuildBankTabInfo(1, "", "INV_Ore");
    CHECK(f.gateway.sent.empty());
    f.service.setGuildBankTabInfo(1, "Ore", "INV_Ore");
    CHECK(f.gateway.sent == std::vector<std::string>{"rename 1 Ore"});
}

TEST_CASE("Items named by a list or a log are looked up", "[guild_bank]") {
    Fixture f;
    GuildBankData d = withTabs(1);
    d.tabs[0].items = {item(0, 11), item(1, 0)};
    f.service.onBankList(d);
    GuildBankLogEntry moved;
    moved.itemId = 22;
    GuildBankLogEntry money;
    money.count = 100;
    f.service.onLog(0, {moved, money});
    CHECK(f.gateway.asked == std::vector<uint32_t>{11, 22});
}

TEST_CASE("The next character is at no banker and has none of the last one's vault", "[guild_bank]") {
    Fixture f;
    GuildBankData d = withTabs(2);
    d.money = 500;
    f.atVault(d);
    f.service.onTabText(0, "Mats");
    f.service.onLog(kMoneyLogTab, {GuildBankLogEntry{}});

    f.service.beginCharacter();

    CHECK_FALSE(f.service.isGuildBankOpen());
    CHECK(f.service.getGuildBankData().money == 0);
    CHECK(f.service.getGuildBankData().tabs.empty());
    CHECK(f.service.getGuildBankTabText(0).empty());
    CHECK(f.service.getGuildBankLog(kMoneyLogTab).empty());
    // No banker, so nothing goes out.
    f.service.queryGuildBankTab(0);
    CHECK(f.gateway.sent.empty());
}
