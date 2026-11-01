#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::guild_bank {

struct GuildBankData;
struct GuildBankLogEntry;

/// The guild vault as the interface and the addon layer use it.
class IGuildBank {
public:
    virtual ~IGuildBank() = default;

    // ---- Reading ----
    [[nodiscard]] virtual bool isGuildBankOpen() const = 0;
    [[nodiscard]] virtual const GuildBankData& getGuildBankData() const = 0;
    [[nodiscard]] virtual uint8_t getGuildBankActiveTab() const = 0;
    [[nodiscard]] virtual const std::string& getGuildBankTabText(uint8_t tabId) const = 0;
    /// A tab's log as the server last sent it; the money log is tab six.
    [[nodiscard]] virtual const std::vector<GuildBankLogEntry>& getGuildBankLog(uint8_t tab) const = 0;

    // ---- The panel ----
    virtual void setGuildBankActiveTab(uint8_t tab) = 0;

    // ---- Requests ----
    virtual void openGuildBank(uint64_t bankerGuid) = 0;
    virtual void closeGuildBank() = 0;
    virtual void queryGuildBankTab(uint8_t tabId) = 0;
    virtual void buyGuildBankTab() = 0;
    virtual void depositGuildBankMoney(uint32_t amount) = 0;
    virtual void withdrawGuildBankMoney(uint32_t amount) = 0;
    virtual void guildBankWithdrawItem(uint8_t tabId, uint8_t bankSlot, uint8_t destBag,
                                       uint8_t destSlot, uint32_t splitCount = 0) = 0;
    virtual void guildBankDepositItem(uint8_t tabId, uint8_t bankSlot, uint8_t srcBag, uint8_t srcSlot) = 0;
    /// Deposits a bag item into the first empty slot of the tab being viewed.
    virtual void guildBankDepositFromInventory(uint8_t srcBag, uint8_t srcSlot) = 0;
    virtual void queryGuildBankText(uint8_t tabId) = 0;
    virtual void setGuildBankTabText(uint8_t tab, const std::string& text) = 0;
    /// Renames a tab and sets its icon; both are needed.
    virtual void setGuildBankTabInfo(uint8_t tabId, const std::string& name, const std::string& icon) = 0;
    virtual void requestGuildBankLog(uint8_t tab) = 0;
};

/// `bank`, or the vault of a client with no game: never open, empty, and
/// every request dropped - for a reader that would otherwise test for it at
/// every call.
[[nodiscard]] IGuildBank& orNoGuildBank(IGuildBank* bank);

/// What the protocol layer and the guild tell the vault.
class IGuildBankFeed {
public:
    virtual ~IGuildBankFeed() = default;

    virtual void onBankList(GuildBankData contents) = 0;
    virtual void onTabText(uint8_t tab, std::string text) = 0;
    virtual void onLog(uint8_t tab, std::vector<GuildBankLogEntry> entries) = 0;
    /// The balance a guild event reports, between bank lists.
    virtual void onMoney(uint64_t money) = 0;
    /// A character is logging in: none of the last one's vault is theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::guild_bank
