#include "loot/domain/loot_window.hpp"

#include <utility>

namespace wowee::loot {

namespace {

constexpr float kMoneyFallbackSeconds = 0.4f;
constexpr float kAnnounceCooldownSeconds = 1.5f;

void append(std::vector<LootChange>& to, std::vector<LootChange> from) {
    for (auto& c : from) to.push_back(std::move(c));
}

} // namespace

std::vector<LootChange> LootWindow::opened(LootContents contents) {
    contents_ = std::move(contents);
    open_ = true;
    sources_[contents_.lootGuid].data = contents_;
    return {LootOpened{autoLoot_}};
}

std::vector<LootChange> LootWindow::closed() {
    if (!open_) return {};
    open_ = false;
    // Gather nodes are not despawned here: the server decides when a node is
    // spent, and a vein can still hold charges for someone else.
    contents_ = LootContents{};
    return {LootClosed{}};
}

std::vector<LootChange> LootWindow::released() {
    sources_.erase(contents_.lootGuid);
    open_ = false;
    contents_ = LootContents{};
    return {LootClosed{}};
}

std::vector<LootChange> LootWindow::slotRemoved(uint8_t slot) {
    for (auto& item : contents_.items) {
        if (item.slotIndex != slot) continue;
        // The receipt is SMSG_ITEM_PUSH_RESULT's to announce; this only empties
        // the button.
        item.looted = true;
        return {LootSlotCleared{slot + 1}};
    }
    return {};
}

std::vector<LootChange> LootWindow::moneyCleared() {
    if (!contents_.goldLeft()) return {};
    contents_.goldLooted = true;
    if (auto it = sources_.find(contents_.lootGuid); it != sources_.end()) it->second.data.goldLooted = true;
    // Display slot one, which is where the coin sits whenever there is one.
    if (open_) return {LootSlotCleared{1}};
    return {};
}

std::vector<LootChange> LootWindow::moneyNotified(uint32_t amount) {
    const uint64_t source = pendingMoneyGuid_ != 0 ? pendingMoneyGuid_ : contents_.lootGuid;
    pendingMoneyGuid_ = 0;
    pendingMoneyAmount_ = 0;
    pendingMoneyTimer_ = 0.0f;
    auto changes = announce(source, amount);
    changes.emplace_back(PurseChanged{});
    // Only for an amount that is this source's whole purse: in a group the
    // notify carries one player's share and reaches everyone, including
    // someone standing over a different corpse. SMSG_LOOT_CLEAR_MONEY names
    // the loot itself and clears the coin in that case.
    if (amount == contents_.gold) append(changes, moneyCleared());
    return changes;
}

void LootWindow::moneyRequested() {
    const uint64_t source = contents_.lootGuid;
    auto cooldown = announceCooldowns_.find(source);
    const bool justAnnounced = cooldown != announceCooldowns_.end() && cooldown->second > 0.0f;
    pendingMoneyGuid_ = justAnnounced ? 0 : source;
    pendingMoneyAmount_ = justAnnounced ? 0 : contents_.gold;
    pendingMoneyTimer_ = justAnnounced ? 0.0f : kMoneyFallbackSeconds;
}

std::vector<LootChange> LootWindow::timePassed(float seconds) {
    // The cooldowns first, so one set by an announcement this frame runs its
    // whole length.
    for (auto it = announceCooldowns_.begin(); it != announceCooldowns_.end();) {
        it->second -= seconds;
        if (it->second <= 0.0f) it = announceCooldowns_.erase(it);
        else ++it;
    }
    std::vector<LootChange> changes;
    if (pendingMoneyTimer_ > 0.0f) {
        pendingMoneyTimer_ -= seconds;
        if (pendingMoneyTimer_ <= 0.0f) {
            pendingMoneyTimer_ = 0.0f;
            changes = announce(pendingMoneyGuid_, pendingMoneyAmount_);
            pendingMoneyGuid_ = 0;
            pendingMoneyAmount_ = 0;
        }
    }
    return changes;
}

std::vector<LootChange> LootWindow::announce(uint64_t sourceGuid, uint32_t amount) {
    if (amount == 0) return {};
    if (auto it = sources_.find(sourceGuid); it != sources_.end()) {
        if (it->second.moneyTaken) return {};
        it->second.moneyTaken = true;
    }
    if (sourceGuid != 0) announceCooldowns_[sourceGuid] = kAnnounceCooldownSeconds;
    return {MoneyAnnounced{sourceGuid, amount}};
}

bool LootWindow::claimAutoStore() {
    auto& source = sources_[contents_.lootGuid];
    if (source.itemsAutoStored) return false;
    source.itemsAutoStored = true;
    return true;
}

std::vector<LootChange> LootWindow::holdForBind(uint8_t slot) {
    // The slot on screen, not the server's: the interface counts the coin as
    // a slot of its own and the items after it.
    int display = contents_.hasCoinSlot() ? 1 : 0;
    for (const auto& item : contents_.items) {
        ++display;
        if (item.slotIndex != slot) continue;
        pendingBind_ = slot;
        return {BindConfirmRequested{display}};
    }
    return {};
}

std::optional<uint8_t> LootWindow::takePendingBind() {
    auto slot = pendingBind_;
    pendingBind_.reset();
    return slot;
}

std::vector<LootChange> LootWindow::masterCandidatesReceived(std::vector<uint64_t> candidates) {
    masterCandidates_ = std::move(candidates);
    return {MasterLootListReceived{}};
}

std::vector<LootChange> LootWindow::itemInfoArrived(uint32_t itemId) const {
    if (!open_) return {};
    std::vector<LootChange> changes;
    const int coinSlots = contents_.hasCoinSlot() ? 1 : 0;
    for (size_t i = 0; i < contents_.items.size(); ++i)
        if (contents_.items[i].itemId == itemId)
            changes.emplace_back(LootSlotChanged{coinSlots + static_cast<int>(i) + 1});
    return changes;
}

const LootItem* LootWindow::item(uint8_t slot) const {
    for (const auto& item : contents_.items)
        if (item.slotIndex == slot) return &item;
    return nullptr;
}

bool LootWindow::heldItems(uint64_t sourceGuid) const {
    auto it = sources_.find(sourceGuid);
    return it != sources_.end() && !it->second.data.items.empty();
}

} // namespace wowee::loot
