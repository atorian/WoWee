#include "loot/domain/loot_rolls.hpp"

#include <utility>

namespace wowee::loot {

std::vector<RollChange> LootRolls::started(LootRoll roll) {
    LootRoll* slot = nullptr;
    for (auto& open : rolls_) {
        if (open.objectGuid == roll.objectGuid && open.slot == roll.slot) {
            roll.rollId = open.rollId;
            slot = &open;
            break;
        }
    }
    if (!slot) {
        roll.rollId = nextRollId_++;
        slot = &rolls_.emplace_back();
    }
    *slot = std::move(roll);
    return {RollStarted{slot->rollId, slot->itemId, slot->itemQuality, slot->itemName, slot->rollCountdownMs}};
}

std::vector<RollChange> LootRolls::ended(uint64_t objectGuid, uint32_t slot) {
    for (auto it = rolls_.begin(); it != rolls_.end(); ++it) {
        if (it->objectGuid != objectGuid || it->slot != slot) continue;
        const int rollId = it->rollId;
        rolls_.erase(it);
        return {RollEnded{rollId}};
    }
    return {};
}

const LootRoll* LootRolls::find(int rollId) const {
    for (const auto& roll : rolls_)
        if (roll.rollId == rollId) return &roll;
    return nullptr;
}

} // namespace wowee::loot
