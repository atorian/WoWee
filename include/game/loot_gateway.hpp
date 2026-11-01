#pragma once

#include "core/event_bus.hpp"
#include "loot/services/ports.hpp"

#include <vector>

namespace wowee::core { class CVarStore; }

namespace wowee::game {

class PacketSender;
class InventoryHandler;
class InventoryQueries;
class ChatLog;
class FrameXmlEvents;

/// The loot service's ports, implemented over the world connection, the item
/// and name caches, the purse and the game object the player used.
class LootGateway final : public loot::ILootGateway, public loot::ILootWorld {
public:
    LootGateway(PacketSender& sender, InventoryHandler& inventory, InventoryQueries& world)
        : sender_(sender), inventory_(inventory), world_(world) {}

    // ILootGateway
    [[nodiscard]] bool online() const override;
    bool mayLoot() override;
    void sendLoot(uint64_t source) override;
    void sendAutostore(uint8_t slot) override;
    void sendLootMoney() override;
    void sendRelease(uint64_t source) override;
    void sendMasterGive(uint64_t source, uint8_t slot, uint64_t target) override;
    void sendRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) override;

    // ILootWorld
    [[nodiscard]] std::optional<ItemFacts> item(uint32_t itemId) const override;
    void requestItem(uint32_t itemId) override;
    [[nodiscard]] std::string playerName(uint64_t guid) const override;
    void creditMoney(uint32_t copper) override;
    [[nodiscard]] bool stillOpening(uint64_t guid) const override;
    bool objectLootArrived(uint64_t guid) override;

private:
    PacketSender& sender_;
    InventoryHandler& inventory_;
    InventoryQueries& world_;
};

/// What the player sees and hears of looting: the LOOT_*, roll and master
/// loot events, PLAYER_MONEY, and the chat lines.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeLootNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                     InventoryHandler& inventory, InventoryQueries& world, const core::CVarStore& cvars);

} // namespace wowee::game
