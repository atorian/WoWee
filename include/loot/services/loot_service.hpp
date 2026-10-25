#pragma once

#include "loot/domain/loot_rolls.hpp"
#include "loot/domain/loot_window.hpp"
#include "loot/services/loot.hpp"
#include "loot/services/ports.hpp"

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::loot {

class LootService final : public ILoot, public ILootFeed {
public:
    LootService(ILootGateway& gateway, ILootWorld& world, core::EventBus& events);

    // ILoot: reading
    [[nodiscard]] bool isLootWindowOpen() const override { return window_.isOpen(); }
    [[nodiscard]] const LootContents& getCurrentLoot() const override { return window_.contents(); }
    [[nodiscard]] bool isAutoLoot() const override { return window_.autoLoot(); }
    [[nodiscard]] const std::vector<uint64_t>& getMasterLootCandidates() const override { return window_.masterCandidates(); }
    [[nodiscard]] const LootRoll* getLootRoll(int rollId) const override { return rolls_.find(rollId); }
    [[nodiscard]] bool corpseHeldItems(uint64_t guid) const override { return window_.heldItems(guid); }

    // ILoot: settings and requests
    void setAutoLoot(bool enabled) override { window_.setAutoLoot(enabled); }
    void lootTarget(uint64_t guid) override;
    void lootItem(uint8_t slotIndex, bool confirmed = false) override;
    void confirmPendingLoot() override;
    void lootMoney() override;
    void closeLoot() override;
    void lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) override;
    void sendLootRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) override;

    // ILootFeed
    void onContents(LootContents contents) override;
    void onReleased() override { apply(window_.released()); }
    void onSlotRemoved(uint8_t slot) override;
    void onMasterCandidates(std::vector<uint64_t> candidates) override;
    void onMoneyNotify(uint32_t amount) override;
    void onMoneyCleared() override;
    void onItemNotify(uint8_t slot) override;
    void onSlotChanged(uint8_t slot) override;
    void onItemInfo(uint32_t itemId) override { apply(window_.itemInfoArrived(itemId)); }
    void onRollStarted(uint64_t objectGuid, uint32_t slot, uint32_t itemId,
                       uint32_t countdownMs, uint8_t voteMask) override;
    void onAllPassed(uint64_t objectGuid, uint32_t slot, uint32_t itemId) override;
    void onVote(uint64_t playerGuid, uint8_t rollNumber, uint8_t rollType) override;
    void onRollWon(uint64_t objectGuid, uint32_t slot, uint32_t itemId, int32_t randomPropertyId,
                   uint64_t winnerGuid, uint8_t rollNumber, uint8_t rollType) override;
    void timePassed(float seconds) override { apply(window_.timePassed(seconds)); }
    void beginCharacter() override {
        window_.beginCharacter();
        rolls_.beginCharacter();
    }

private:
    /// An emptied window closes itself: the interface's closes only on
    /// LOOT_CLOSED, and autoloot clears every slot without a close.
    void closeIfEmptied();
    [[nodiscard]] std::string playerNameOr(uint64_t guid) const;
    template <typename Changes> void apply(const Changes& changes);

    ILootGateway& gateway_;
    ILootWorld& world_;
    core::EventBus& events_;
    LootWindow window_;
    LootRolls rolls_;
};

} // namespace wowee::loot
