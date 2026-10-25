#pragma once

#include <cstdint>
#include <vector>

namespace wowee::loot {

struct LootContents;
struct LootRoll;

/// Looting as the interface, the addon layer and the rest of the game use it.
class ILoot {
public:
    virtual ~ILoot() = default;

    // ---- Reading ----
    [[nodiscard]] virtual bool isLootWindowOpen() const = 0;
    [[nodiscard]] virtual const LootContents& getCurrentLoot() const = 0;
    [[nodiscard]] virtual bool isAutoLoot() const = 0;
    [[nodiscard]] virtual const std::vector<uint64_t>& getMasterLootCandidates() const = 0;
    /// The open roll the interface knows by this id, or null.
    [[nodiscard]] virtual const LootRoll* getLootRoll(int rollId) const = 0;
    /// Whether a corpse held items when it was last opened: such a corpse is
    /// still worth tabbing to out of combat.
    [[nodiscard]] virtual bool corpseHeldItems(uint64_t guid) const = 0;

    // ---- Settings ----
    virtual void setAutoLoot(bool enabled) = 0;

    // ---- Requests ----
    virtual void lootTarget(uint64_t guid) = 0;
    /// Takes an item. One that binds on pickup asks first unless `confirmed`.
    virtual void lootItem(uint8_t slotIndex, bool confirmed = false) = 0;
    /// Sends the item request held back for the bind prompt.
    virtual void confirmPendingLoot() = 0;
    virtual void lootMoney() = 0;
    virtual void closeLoot() = 0;
    virtual void lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) = 0;
    /// 0 pass, 1 need, 2 greed, 3 disenchant.
    virtual void sendLootRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) = 0;
};

/// `loot`, or the loot of a client with no game: never open, empty, and every
/// request dropped - for a reader that would otherwise test for it at every
/// call.
[[nodiscard]] ILoot& orNoLoot(ILoot* loot);

/// What the protocol layer tells the loot window and the rolls.
class ILootFeed {
public:
    virtual ~ILootFeed() = default;

    virtual void onContents(LootContents contents) = 0;
    virtual void onReleased() = 0;
    virtual void onSlotRemoved(uint8_t slot) = 0;
    virtual void onMasterCandidates(std::vector<uint64_t> candidates) = 0;
    /// SMSG_LOOT_MONEY_NOTIFY: money that went into the purse.
    virtual void onMoneyNotify(uint32_t amount) = 0;
    virtual void onMoneyCleared() = 0;
    /// SMSG_LOOT_ITEM_NOTIFY: an item from this loot went to the player.
    virtual void onItemNotify(uint8_t slot) = 0;
    virtual void onSlotChanged(uint8_t slot) = 0;
    /// An item template arrived from the server.
    virtual void onItemInfo(uint32_t itemId) = 0;
    virtual void onRollStarted(uint64_t objectGuid, uint32_t slot, uint32_t itemId,
                               uint32_t countdownMs, uint8_t voteMask) = 0;
    virtual void onAllPassed(uint64_t objectGuid, uint32_t slot, uint32_t itemId) = 0;
    virtual void onVote(uint64_t playerGuid, uint8_t rollNumber, uint8_t rollType) = 0;
    virtual void onRollWon(uint64_t objectGuid, uint32_t slot, uint32_t itemId, int32_t randomPropertyId,
                           uint64_t winnerGuid, uint8_t rollNumber, uint8_t rollType) = 0;
    virtual void timePassed(float seconds) = 0;
    /// A character is logging in: none of the last one's loot or rolls is
    /// theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::loot
