#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::guild_bank {

struct GuildBankItemSlot {
    uint8_t slotId = 0;
    uint32_t itemEntry = 0;
    uint32_t stackCount = 1;
    uint32_t enchantId = 0;
    uint32_t randomPropertyId = 0;
};

struct GuildBankTab {
    std::string tabName;
    std::string tabIcon;
    std::vector<GuildBankItemSlot> items;
};

/// SMSG_GUILD_BANK_LIST: the vault's money, the withdrawal allowance, and one
/// tab's contents - with the tab names when they changed.
struct GuildBankData {
    uint64_t money = 0;
    uint8_t tabId = 0;
    int32_t withdrawAmount = -1;  // -1 = unlimited
    std::vector<GuildBankTab> tabs;  // Only populated on fullUpdate
    std::vector<GuildBankItemSlot> tabItems;  // Current tab items
    /// Whether the list just parsed carried the tab names, which is what the
    /// panel's GUILDBANK_UPDATE_TABS means. A slots-only update leaves `tabs`
    /// as it found them, so their contents cannot answer this.
    bool tabsIncluded = false;
};

/// One line of a bank tab's log, or of the money tab's.
///
/// Types are AzerothCore's GuildBankEventLogTypes: 1 deposit item, 2 withdraw
/// item, 3 move item, 4 deposit money, 5 withdraw money, 6 repair, 7 move item
/// between tabs, 9 buy a tab. Which fields carry anything depends on which.
struct GuildBankLogEntry {
    uint8_t  type = 0;
    uint64_t playerGuid = 0;
    uint32_t itemId = 0;      ///< item types only
    uint32_t count = 0;       ///< stack size for items, copper for money
    uint8_t  otherTab = 0;    ///< a move's destination
    uint32_t secondsAgo = 0;
};

/// Six item tabs; the log after them, numbered six, is the money log, which
/// is what the server means by GUILD_BANK_MAX_TABS.
inline constexpr uint8_t kItemTabs = 6;
inline constexpr uint8_t kMoneyLogTab = 6;
inline constexpr int kTabSlots = 98;  // GUILD_BANK_MAX_SLOTS

} // namespace wowee::guild_bank
