#include "guild_bank/domain/guild_bank.hpp"

namespace wowee::guild_bank {

void GuildBank::activationSent(uint64_t bankerGuid) {
    banker_ = bankerGuid;
    activeTab_ = 0;
}

std::vector<VaultChange> GuildBank::closed() {
    open_ = false;
    banker_ = 0;
    return {VaultClosed{}};
}

std::vector<VaultChange> GuildBank::contentsReceived(GuildBankData contents) {
    contents_ = std::move(contents);
    std::vector<VaultChange> changes;
    if (banker_ != 0 && !open_) {
        open_ = true;
        changes.emplace_back(VaultOpened{});
    }
    // Each list names the tab it describes; clicking a tab only asks for it.
    activeTab_ = contents_.tabId;
    // Only when names arrived: a slot refresh does not change the set of tabs,
    // and the event is what makes the panel pick a tab to draw.
    if (contents_.tabsIncluded && !contents_.tabs.empty()) changes.emplace_back(TabsChanged{});
    changes.emplace_back(SlotsChanged{});
    changes.emplace_back(MoneyChanged{});
    changes.emplace_back(WithdrawAllowanceChanged{});
    return changes;
}

std::vector<VaultChange> GuildBank::moneyReported(uint64_t money) {
    if (contents_.money == money) return {};
    contents_.money = money;
    return {MoneyChanged{}};
}

std::vector<VaultChange> GuildBank::tabTextReceived(uint8_t tab, std::string text) {
    if (tab < tabTexts_.size()) tabTexts_[tab] = std::move(text);
    return {TabTextChanged{tab + 1}};
}

std::vector<VaultChange> GuildBank::logReceived(uint8_t tab, std::vector<GuildBankLogEntry> entries) {
    if (tab >= logs_.size()) return {};
    logs_[tab] = std::move(entries);
    return {LogChanged{}};
}

std::optional<uint8_t> GuildBank::freeSlot() const {
    std::array<bool, kTabSlots> taken{};
    for (const auto& slot : contents_.tabItems)
        if (slot.itemEntry != 0 && slot.slotId < kTabSlots) taken[slot.slotId] = true;
    for (int s = 0; s < kTabSlots; ++s)
        if (!taken[static_cast<size_t>(s)]) return static_cast<uint8_t>(s);
    return std::nullopt;
}

const std::string& GuildBank::tabText(uint8_t tab) const {
    static const std::string kNone;
    return tab < tabTexts_.size() ? tabTexts_[tab] : kNone;
}

const std::vector<GuildBankLogEntry>& GuildBank::log(uint8_t tab) const {
    static const std::vector<GuildBankLogEntry> kNone;
    return tab < logs_.size() ? logs_[tab] : kNone;
}

} // namespace wowee::guild_bank
