#include "loot/services/loot_service.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "loot/domain/records.hpp"

namespace wowee::loot {

namespace {

/// The loot of a client with no game: never open, empty, and every request
/// dropped.
class NoLoot final : public ILoot {
public:
    bool isLootWindowOpen() const override { return false; }
    const LootContents& getCurrentLoot() const override { return contents_; }
    bool isAutoLoot() const override { return false; }
    const std::vector<uint64_t>& getMasterLootCandidates() const override { return candidates_; }
    const LootRoll* getLootRoll(int) const override { return nullptr; }
    bool corpseHeldItems(uint64_t) const override { return false; }
    void setAutoLoot(bool) override {}
    void lootTarget(uint64_t) override {}
    void lootItem(uint8_t, bool) override {}
    void confirmPendingLoot() override {}
    void lootMoney() override {}
    void closeLoot() override {}
    void lootMasterGive(uint8_t, uint64_t) override {}
    void sendLootRoll(uint64_t, uint32_t, uint8_t) override {}

private:
    LootContents contents_;
    std::vector<uint64_t> candidates_;
};

NoLoot kNoLoot;

} // namespace

ILoot& orNoLoot(ILoot* loot) {
    return loot ? *loot : kNoLoot;
}

LootService::LootService(ILootGateway& gateway, ILootWorld& world, core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

template <typename Changes>
void LootService::apply(const Changes& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void LootService::closeIfEmptied() {
    if (window_.emptied()) closeLoot();
}

std::string LootService::playerNameOr(uint64_t guid) const {
    std::string name = world_.playerName(guid);
    return name.empty() ? std::string("Player") : name;
}

void LootService::lootTarget(uint64_t guid) {
    if (!gateway_.mayLoot()) return;
    window_.targetLooted();
    gateway_.sendLoot(guid);
}

void LootService::lootItem(uint8_t slotIndex, bool confirmed) {
    if (!gateway_.mayLoot()) return;
    if (!confirmed) {
        if (const auto* item = window_.item(slotIndex)) {
            const auto facts = world_.item(item->itemId);
            if (facts && facts->bindsOnPickup) {
                apply(window_.holdForBind(slotIndex));
                return;
            }
        }
    }
    window_.itemRequested();
    gateway_.sendAutostore(slotIndex);
}

void LootService::confirmPendingLoot() {
    if (const auto slot = window_.takePendingBind()) lootItem(*slot, true);
}

void LootService::lootMoney() {
    if (gateway_.online()) gateway_.sendLootMoney();
}

void LootService::closeLoot() {
    if (!window_.isOpen()) return;
    if (gateway_.online()) gateway_.sendRelease(window_.contents().lootGuid);
    apply(window_.closed());
}

void LootService::lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) {
    if (gateway_.online()) gateway_.sendMasterGive(window_.contents().lootGuid, lootSlot, targetGuid);
}

void LootService::sendLootRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) {
    if (!gateway_.online()) return;
    gateway_.sendRoll(objectGuid, slot, rollType);
    // Any choice closes this roll's window, and only this roll's.
    apply(rolls_.ended(objectGuid, slot));
}

void LootService::onContents(LootContents contents) {
    const uint64_t source = contents.lootGuid;
    const bool hasLoot = !contents.items.empty() || contents.gold > 0;
    // A node being gathered answers empty before the cast completes.
    if (!hasLoot && world_.stillOpening(source)) {
        LOG_DEBUG("Ignoring empty SMSG_LOOT_RESPONSE during pending gather/open");
        return;
    }

    apply(window_.opened(std::move(contents)));
    const bool usedObject = world_.objectLootArrived(source);
    const auto& loot = window_.contents();
    for (const auto& item : loot.items) world_.requestItem(item.itemId);

    // The coin stays in the window until the server says it is taken: the
    // interface built its buttons on LOOT_OPENED with the coin as slot one.
    if (loot.gold > 0 && gateway_.online()) {
        window_.moneyRequested();
        gateway_.sendLootMoney();
    }

    // A used game object is a single-action pickup, so its items are stored
    // even with autoloot off; otherwise a quest container opened and granted
    // nothing.
    if ((window_.autoLoot() || usedObject) && gateway_.online() && window_.claimAutoStore()) {
        for (const auto& item : loot.items) {
            LOG_INFO("Autostoring loot slot=", static_cast<int>(item.slotIndex), " item=", item.itemId,
                     " count=", item.count, " fromGuid=0x", std::hex, loot.lootGuid, std::dec,
                     " gameObject=", usedObject);
            gateway_.sendAutostore(item.slotIndex);
        }
    }

    // Only money, or nothing at all: empty already. An item loot closes once
    // the server has cleared its last slot; releasing before would drop them.
    if (window_.isOpen() && loot.items.empty() && loot.gold == 0) closeLoot();
}

void LootService::onSlotRemoved(uint8_t slot) {
    apply(window_.slotRemoved(slot));
    closeIfEmptied();
}

void LootService::onMasterCandidates(std::vector<uint64_t> candidates) {
    apply(window_.masterCandidatesReceived(std::move(candidates)));
}

void LootService::onMoneyNotify(uint32_t amount) {
    world_.creditMoney(amount);
    apply(window_.moneyNotified(amount));
    closeIfEmptied();
}

void LootService::onMoneyCleared() {
    apply(window_.moneyCleared());
    closeIfEmptied();
}

void LootService::onItemNotify(uint8_t slot) {
    const auto* item = window_.item(slot);
    if (item && item->itemId != 0) events_.publish(LootItemReceived{item->itemId});
}

void LootService::onSlotChanged(uint8_t slot) {
    events_.publish(LootSlotChanged{slot + 1});
}

void LootService::onRollStarted(uint64_t objectGuid, uint32_t slot, uint32_t itemId,
                                uint32_t countdownMs, uint8_t voteMask) {
    world_.requestItem(itemId);
    const auto facts = world_.item(itemId);
    LootRoll roll;
    roll.objectGuid = objectGuid;
    roll.slot = slot;
    roll.itemId = itemId;
    roll.itemName = (facts && !facts->name.empty()) ? facts->name : "Item #" + std::to_string(itemId);
    roll.itemQuality = static_cast<uint8_t>(facts ? facts->quality : 1);
    roll.rollCountdownMs = countdownMs;
    roll.voteMask = voteMask;
    roll.rollStartedAt = std::chrono::steady_clock::now();
    apply(rolls_.started(std::move(roll)));
}

void LootService::onAllPassed(uint64_t objectGuid, uint32_t slot, uint32_t itemId) {
    world_.requestItem(itemId);
    events_.publish(EveryonePassed{itemId});
    apply(rolls_.ended(objectGuid, slot));
}

void LootService::onVote(uint64_t playerGuid, uint8_t rollNumber, uint8_t rollType) {
    events_.publish(VoteCast{playerNameOr(playerGuid), rollNumber, rollType});
}

void LootService::onRollWon(uint64_t objectGuid, uint32_t slot, uint32_t itemId, int32_t randomPropertyId,
                            uint64_t winnerGuid, uint8_t rollNumber, uint8_t rollType) {
    const std::string winner = playerNameOr(winnerGuid);
    world_.requestItem(itemId);
    events_.publish(RollWon{winner, itemId, randomPropertyId, rollNumber, rollType});
    apply(rolls_.ended(objectGuid, slot));
}

} // namespace wowee::loot
