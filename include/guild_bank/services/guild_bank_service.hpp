#pragma once

#include "guild_bank/domain/guild_bank.hpp"
#include "guild_bank/services/guild_bank.hpp"
#include "guild_bank/services/ports.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::guild_bank {

class GuildBankService final : public IGuildBank, public IGuildBankFeed {
public:
    GuildBankService(IGuildBankGateway& gateway, IGuildBankWorld& world, core::EventBus& events);

    // IGuildBank: reading
    [[nodiscard]] bool isGuildBankOpen() const override { return bank_.isOpen(); }
    [[nodiscard]] const GuildBankData& getGuildBankData() const override { return bank_.contents(); }
    [[nodiscard]] uint8_t getGuildBankActiveTab() const override { return bank_.activeTab(); }
    [[nodiscard]] const std::string& getGuildBankTabText(uint8_t tabId) const override { return bank_.tabText(tabId); }
    [[nodiscard]] const std::vector<GuildBankLogEntry>& getGuildBankLog(uint8_t tab) const override { return bank_.log(tab); }

    // IGuildBank: the panel
    void setGuildBankActiveTab(uint8_t tab) override { bank_.selectTab(tab); }

    // IGuildBank: requests
    void openGuildBank(uint64_t bankerGuid) override;
    void closeGuildBank() override { apply(bank_.closed()); }
    void queryGuildBankTab(uint8_t tabId) override;
    void buyGuildBankTab() override;
    void depositGuildBankMoney(uint32_t amount) override;
    void withdrawGuildBankMoney(uint32_t amount) override;
    void guildBankWithdrawItem(uint8_t tabId, uint8_t bankSlot, uint8_t destBag,
                               uint8_t destSlot, uint32_t splitCount = 0) override;
    void guildBankDepositItem(uint8_t tabId, uint8_t bankSlot, uint8_t srcBag, uint8_t srcSlot) override;
    void guildBankDepositFromInventory(uint8_t srcBag, uint8_t srcSlot) override;
    void queryGuildBankText(uint8_t tabId) override;
    void setGuildBankTabText(uint8_t tab, const std::string& text) override;
    void setGuildBankTabInfo(uint8_t tabId, const std::string& name, const std::string& icon) override;
    void requestGuildBankLog(uint8_t tab) override;

    // IGuildBankFeed
    void onBankList(GuildBankData contents) override;
    void onTabText(uint8_t tab, std::string text) override { apply(bank_.tabTextReceived(tab, std::move(text))); }
    void onLog(uint8_t tab, std::vector<GuildBankLogEntry> entries) override;
    void onMoney(uint64_t money) override { apply(bank_.moneyReported(money)); }
    void beginCharacter() override { bank_.beginCharacter(); }

private:
    /// Online and at a banker.
    [[nodiscard]] bool atBanker() const;
    void apply(const std::vector<VaultChange>& changes);

    IGuildBankGateway& gateway_;
    IGuildBankWorld& world_;
    core::EventBus& events_;
    GuildBank bank_;
};

} // namespace wowee::guild_bank
