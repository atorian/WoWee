#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::loot {

// What happened at the loot window and to the group's rolls. LootWindow and
// LootRolls return these; the service publishes them, and the chat log, the
// addon layer, the sound and the player's animation subscribe.

/// The window opened on what a source holds (LOOT_OPENED); `autoLoot` says
/// whether it is being taken without the player.
struct LootOpened { bool autoLoot = false; };
struct LootClosed {};
/// One button emptied (LOOT_SLOT_CLEARED), in the interface's numbering.
struct LootSlotCleared { int slot = 0; };
/// A loot button needs redrawing (LOOT_SLOT_CHANGED), in the interface's
/// numbering.
struct LootSlotChanged { int slot = 0; };
/// An item that binds when picked up, held back until the player agrees
/// (LOOT_BIND_CONFIRM). `slot` is the one on screen, where the coin counts.
struct BindConfirmRequested { int slot = 0; };
/// Money taken from a source, said once per source.
struct MoneyAnnounced { uint64_t lootGuid = 0; uint32_t copper = 0; };
/// The player's purse changed (PLAYER_MONEY).
struct PurseChanged {};
/// Who a master looter can give to, which opens and fills the menu.
struct MasterLootListReceived {};

using LootChange = std::variant<LootOpened, LootClosed, LootSlotCleared, LootSlotChanged, BindConfirmRequested,
                                MoneyAnnounced, PurseChanged, MasterLootListReceived>;

/// A roll opened, with the id the interface will know it by (START_LOOT_ROLL).
struct RollStarted {
    int rollId = 0;
    uint32_t itemId = 0;
    uint8_t quality = 0;
    std::string itemName;
    uint32_t countdownMs = 0;
};
/// A roll closed, and the window it opened with it (CANCEL_LOOT_ROLL).
struct RollEnded { int rollId = 0; };

using RollChange = std::variant<RollStarted, RollEnded>;

// What the server reported, published by the service as it arrives.

/// An item from the open loot went to the player.
struct LootItemReceived { uint32_t itemId = 0; };
/// Someone rolled, or passed. 0 pass, 1 need, 2 greed, 3 disenchant.
struct VoteCast { std::string playerName; uint8_t rollNumber = 0; uint8_t rollType = 0; };
struct EveryonePassed { uint32_t itemId = 0; };
struct RollWon {
    std::string winnerName;
    uint32_t itemId = 0;
    int32_t randomPropertyId = 0;
    uint8_t rollNumber = 0;
    uint8_t rollType = 0;
};

} // namespace wowee::loot
