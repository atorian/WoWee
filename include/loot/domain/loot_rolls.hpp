#pragma once

#include "loot/domain/events.hpp"
#include "loot/domain/records.hpp"

#include <vector>

namespace wowee::loot {

/// Every group roll open at once: a boss puts several items up together.
class LootRolls {
public:
    /// A roll already open on the same item of the same corpse is restated,
    /// keeping its id: the server restates a roll when its countdown resets,
    /// and two windows for one item is worse than none.
    std::vector<RollChange> started(LootRoll roll);
    std::vector<RollChange> ended(uint64_t objectGuid, uint32_t slot);

    [[nodiscard]] const LootRoll* find(int rollId) const;
    /// A character is logging in: none of the last one's rolls is theirs.
    void beginCharacter() { *this = LootRolls{}; }

private:
    std::vector<LootRoll> rolls_;
    int nextRollId_ = 1;
};

} // namespace wowee::loot
