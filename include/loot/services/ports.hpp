#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace wowee::loot {

/// Requests to the server about loot. Implemented by the protocol layer.
class ILootGateway {
public:
    virtual ~ILootGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    /// Whether looting may be asked for now. With the world connection lost
    /// and being restored, the player is told so.
    virtual bool mayLoot() = 0;

    virtual void sendLoot(uint64_t source) = 0;
    virtual void sendAutostore(uint8_t slot) = 0;
    virtual void sendLootMoney() = 0;
    virtual void sendRelease(uint64_t source) = 0;
    virtual void sendMasterGive(uint64_t source, uint8_t slot, uint64_t target) = 0;
    virtual void sendRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) = 0;
};

/// What loot needs from the rest of the game.
class ILootWorld {
public:
    virtual ~ILootWorld() = default;

    struct ItemFacts {
        std::string name;
        uint32_t quality = 1;
        bool bindsOnPickup = false;
    };
    /// What the item cache knows about an item, if it has it.
    [[nodiscard]] virtual std::optional<ItemFacts> item(uint32_t itemId) const = 0;
    virtual void requestItem(uint32_t itemId) = 0;
    /// The name, empty while it is not known.
    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    /// Money that went into the purse.
    virtual void creditMoney(uint32_t copper) = 0;

    /// The game object the player used is still being gathered or opened, so
    /// an empty answer from it is not its contents yet.
    [[nodiscard]] virtual bool stillOpening(uint64_t guid) const = 0;
    /// A loot window opened on this source. True when it is the game object
    /// the player used, whose use is now over.
    virtual bool objectLootArrived(uint64_t guid) = 0;
};

} // namespace wowee::loot
