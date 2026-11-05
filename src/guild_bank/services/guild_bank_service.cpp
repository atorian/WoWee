#include "guild_bank/services/guild_bank_service.hpp"

#include "foundation/event_bus.hpp"

namespace wowee::guild_bank {

namespace {

/// The vault of a client with no game: never open, empty, and every request
/// dropped.
class NoGuildBank final : public IGuildBank {
public:
    bool isGuildBankOpen() const override { return false; }
    const GuildBankData& getGuildBankData() const override { return data_; }
    uint8_t getGuildBankActiveTab() const override { return 0; }
    const std::string& getGuildBankTabText(uint8_t) const override { return text_; }
    const std::vector<GuildBankLogEntry>& getGuildBankLog(uint8_t) const override { return log_; }
    void setGuildBankActiveTab(uint8_t) override {}
    void openGuildBank(uint64_t) override {}
    void closeGuildBank() override {}
    void queryGuildBankTab(uint8_t) override {}
    void buyGuildBankTab() override {}
    void depositGuildBankMoney(uint32_t) override {}
    void withdrawGuildBankMoney(uint32_t) override {}
    void guildBankWithdrawItem(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t) override {}
    void guildBankDepositItem(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void guildBankDepositFromInventory(uint8_t, uint8_t) override {}
    void queryGuildBankText(uint8_t) override {}
    void setGuildBankTabText(uint8_t, const std::string&) override {}
    void setGuildBankTabInfo(uint8_t, const std::string&, const std::string&) override {}
    void requestGuildBankLog(uint8_t) override {}

private:
    GuildBankData data_;
    std::string text_;
    std::vector<GuildBankLogEntry> log_;
};

NoGuildBank kNoGuildBank;

} // namespace

IGuildBank& orNoGuildBank(IGuildBank* bank) {
    return bank ? *bank : kNoGuildBank;
}

GuildBankService::GuildBankService(IGuildBankGateway& gateway, IGuildBankWorld& world, core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

bool GuildBankService::atBanker() const {
    return gateway_.online() && bank_.banker() != 0;
}

void GuildBankService::apply(const std::vector<VaultChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void GuildBankService::openGuildBank(uint64_t bankerGuid) {
    // A vault belongs to a guild. The server answers a guildless player's
    // activation with an error and no list, so a window raised here would have
    // nothing behind it and never would.
    if (!world_.inGuild()) {
        events_.publish(OpenRefused{});
        return;
    }
    bank_.activationSent(bankerGuid);
    if (gateway_.online() && bankerGuid != 0) gateway_.sendActivate(bankerGuid);
    queryGuildBankTab(0);
}

void GuildBankService::queryGuildBankTab(uint8_t tabId) {
    if (atBanker()) gateway_.sendQueryTab(bank_.banker(), tabId);
}

void GuildBankService::buyGuildBankTab() {
    if (atBanker()) gateway_.sendBuyTab(bank_.banker(), bank_.nextTab());
}

void GuildBankService::depositGuildBankMoney(uint32_t amount) {
    if (atBanker()) gateway_.sendDepositMoney(bank_.banker(), amount);
}

void GuildBankService::withdrawGuildBankMoney(uint32_t amount) {
    if (atBanker()) gateway_.sendWithdrawMoney(bank_.banker(), amount);
}

void GuildBankService::guildBankWithdrawItem(uint8_t tabId, uint8_t bankSlot, uint8_t destBag,
                                             uint8_t destSlot, uint32_t splitCount) {
    if (atBanker()) gateway_.sendWithdrawItem(bank_.banker(), tabId, bankSlot, destBag, destSlot, splitCount);
}

void GuildBankService::guildBankDepositItem(uint8_t tabId, uint8_t bankSlot, uint8_t srcBag, uint8_t srcSlot) {
    if (atBanker()) gateway_.sendDepositItem(bank_.banker(), tabId, bankSlot, srcBag, srcSlot);
}

void GuildBankService::guildBankDepositFromInventory(uint8_t srcBag, uint8_t srcSlot) {
    if (!atBanker() || !bank_.isOpen()) return;
    const auto slot = bank_.freeSlot();
    if (!slot) {
        events_.publish(TabFull{});
        return;
    }
    gateway_.sendDepositItem(bank_.banker(), bank_.activeTab(), *slot, srcBag, srcSlot);
}

void GuildBankService::queryGuildBankText(uint8_t tabId) {
    if (gateway_.online()) gateway_.sendQueryText(tabId);
}

void GuildBankService::setGuildBankTabText(uint8_t tab, const std::string& text) {
    if (gateway_.online()) gateway_.sendTabText(tab, text);
}

void GuildBankService::setGuildBankTabInfo(uint8_t tabId, const std::string& name, const std::string& icon) {
    // Both or neither: the server tests them before it does anything, so a
    // half-filled request is a packet that changes nothing and says nothing.
    if (name.empty() || icon.empty()) return;
    if (atBanker()) gateway_.sendUpdateTab(bank_.banker(), tabId, name, icon);
}

void GuildBankService::requestGuildBankLog(uint8_t tab) {
    if (gateway_.online()) gateway_.sendLogQuery(tab);
}

void GuildBankService::onBankList(GuildBankData contents) {
    apply(bank_.contentsReceived(std::move(contents)));
    for (const auto& tab : bank_.contents().tabs)
        for (const auto& item : tab.items)
            if (item.itemEntry != 0) world_.requestItem(item.itemEntry);
}

void GuildBankService::onLog(uint8_t tab, std::vector<GuildBankLogEntry> entries) {
    for (const auto& e : entries)
        if (e.itemId != 0) world_.requestItem(e.itemId);
    apply(bank_.logReceived(tab, std::move(entries)));
}

} // namespace wowee::guild_bank
