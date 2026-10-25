#pragma once

#include "guild_bank/domain/events.hpp"
#include "guild_bank/domain/records.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::guild_bank {

/// The guild vault the player is at: the tab being viewed and its contents,
/// the tab texts, the money, and the logs the server last sent.
class GuildBank {
public:
    /// A banker was activated. The window opens when the vault answers, not
    /// when it is asked: a refusal would otherwise leave an empty vault up.
    void activationSent(uint64_t bankerGuid);
    std::vector<VaultChange> closed();
    /// A character is logging in: no banker, tab, text or log is theirs.
    void beginCharacter() { *this = GuildBank{}; }

    /// A tab list arrived: the server agreeing that this player may look.
    std::vector<VaultChange> contentsReceived(GuildBankData contents);
    /// The balance as a guild event reports it.
    std::vector<VaultChange> moneyReported(uint64_t money);
    std::vector<VaultChange> tabTextReceived(uint8_t tab, std::string text);
    std::vector<VaultChange> logReceived(uint8_t tab, std::vector<GuildBankLogEntry> entries);

    void selectTab(uint8_t tab) { activeTab_ = tab; }

    /// Where a deposit from the bags goes: the first empty slot of the tab
    /// being viewed, as the retail client's right-click does. The server
    /// picks no slot for that direction.
    [[nodiscard]] std::optional<uint8_t> freeSlot() const;
    /// The tab a purchase would add.
    [[nodiscard]] uint8_t nextTab() const { return static_cast<uint8_t>(contents_.tabs.size()); }

    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] uint64_t banker() const { return banker_; }
    [[nodiscard]] uint8_t activeTab() const { return activeTab_; }
    [[nodiscard]] const GuildBankData& contents() const { return contents_; }
    [[nodiscard]] const std::string& tabText(uint8_t tab) const;
    [[nodiscard]] const std::vector<GuildBankLogEntry>& log(uint8_t tab) const;

private:
    bool open_ = false;
    uint64_t banker_ = 0;
    uint8_t activeTab_ = 0;
    GuildBankData contents_;
    std::array<std::string, kItemTabs> tabTexts_{};
    std::array<std::vector<GuildBankLogEntry>, kItemTabs + 1> logs_{};
};

} // namespace wowee::guild_bank
