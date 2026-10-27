#include "reputation/domain/factions.hpp"

namespace wowee::reputation {

void FactionCatalog::describe(const std::vector<FactionRecord>& rows) {
    for (const auto& r : rows) {
        if (r.id == 0) continue;
        if (!r.name.empty()) names_[r.id] = r.name;
        if (r.parentId != 0 && r.parentId != r.id) parents_[r.id] = r.parentId;
        if (r.hasBase) base_[r.id] = r.base;
        if (r.reputationIndex != kNoReputationIndex) {
            byIndex_[r.reputationIndex] = r.id;
            indexOf_[r.id] = r.reputationIndex;
        }
    }
}

const std::string& FactionCatalog::name(uint32_t factionId) const {
    static const std::string kEmpty;
    auto it = names_.find(factionId);
    return it != names_.end() ? it->second : kEmpty;
}

uint32_t FactionCatalog::parentOf(uint32_t factionId) const {
    auto it = parents_.find(factionId);
    return it != parents_.end() ? it->second : 0u;
}

uint32_t FactionCatalog::factionAt(uint32_t reputationIndex) const {
    auto it = byIndex_.find(reputationIndex);
    return it != byIndex_.end() ? it->second : 0u;
}

uint32_t FactionCatalog::reputationIndexOf(uint32_t factionId) const {
    auto it = indexOf_.find(factionId);
    return it != indexOf_.end() ? it->second : kNoReputationIndex;
}

int32_t FactionCatalog::baseStanding(uint32_t factionId, uint8_t race, uint8_t playerClass) const {
    auto it = base_.find(factionId);
    if (it == base_.end()) return 0;
    const uint32_t raceMask = race ? (1u << (race - 1)) : 0;
    const uint32_t classMask = playerClass ? (1u << (playerClass - 1)) : 0;
    const auto& f = it->second;
    for (int slot = 0; slot < 4; ++slot) {
        const auto slotRaces = static_cast<uint32_t>(f[slot]);
        const auto slotClasses = static_cast<uint32_t>(f[4 + slot]);
        if (((slotRaces & raceMask) != 0 || (slotRaces == 0 && slotClasses != 0)) &&
            ((slotClasses & classMask) != 0 || slotClasses == 0)) {
            return f[8 + slot];
        }
    }
    return 0;
}

} // namespace wowee::reputation
