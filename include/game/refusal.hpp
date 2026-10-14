#pragma once

#include <cstdint>

namespace wowee::game {

/// Why the game refused something the player asked for - the reasons the
/// character answers aloud ("Not enough mana", "My bags are full").
///
/// The game's own words for it. Which voice line, if any, answers a reason is
/// the client's to decide; the game says only why.
enum class Refusal : uint8_t {
    NoMana,
    NoRage,
    NoEnergy,
    OutOfRange,
    NoTarget,
    InvalidAttackTarget,
    SpellCooldown,
    AbilityCooldown,
    ItemCooldown,
    NoAmmo,
    AmmoOnly,
    ChestInUse,
    InventoryFull,
    BagFull,
    ItemMaxCount,
    NotABag,
    CantUseItem,
    ItemLocked,
    NotEquippable,
    CantEquipLevel,
    CantEquipEver,
    CantDropSoulbound,
    NotEnoughMoney,
    CantLearnSpell,
};

} // namespace wowee::game
