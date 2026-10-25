#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::loot {

struct LootItem {
    uint8_t slotIndex = 0;
    uint32_t itemId = 0;
    uint32_t count = 0;
    uint32_t displayInfoId = 0;
    uint32_t randomSuffix = 0;
    uint32_t randomPropertyId = 0;
    uint8_t lootSlotType = 0;
    bool isQuestItem = false;
    bool looted = false;
};

/// What a corpse, a chest or a node holds, as SMSG_LOOT_RESPONSE lists it.
struct LootContents {
    uint64_t lootGuid = 0;
    uint8_t lootType = 0;
    uint32_t gold = 0;           // In copper
    bool goldLooted = false;     // Taken; the coin keeps its slot
    std::vector<LootItem> items;

    /// Whether display slot one is the coin, taken or not.
    [[nodiscard]] bool hasCoinSlot() const { return gold > 0; }
    [[nodiscard]] bool goldLeft() const { return gold > 0 && !goldLooted; }

    [[nodiscard]] bool nothingLeft() const {
        if (goldLeft()) return false;
        for (const auto& item : items)
            if (!item.looted) return false;
        return true;
    }
};

/// A group roll on one item of one corpse.
struct LootRoll {
    /// What the interface calls this roll, and what it passes back to
    /// RollOnLoot. Its own number rather than the loot slot: a dungeon rolls
    /// several items at once and two of them can sit in the same slot of
    /// different corpses, so the slot does not tell one roll from another.
    int      rollId        = 0;
    uint64_t objectGuid    = 0;
    uint32_t slot          = 0;
    uint32_t itemId        = 0;
    std::string itemName;
    uint8_t  itemQuality   = 0;
    uint32_t rollCountdownMs = 60000;
    /// Which of need, greed and disenchant the server will accept.
    uint8_t  voteMask      = 0xFF;
    std::chrono::steady_clock::time_point rollStartedAt{};
};

} // namespace wowee::loot
