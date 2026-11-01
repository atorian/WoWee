#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::pet {

/// The pet bar. Each slot is a packed action: the spell or command in the low
/// 24 bits, its type in the top byte (see actions.hpp).
inline constexpr int kActionBarSlots = 10;

/// What the pet is told to do and how it answers attackers.
struct PetMode {
    uint8_t react = 1;    ///< 0 passive, 1 defensive, 2 aggressive
    uint8_t command = 1;  ///< 0 stay, 1 follow, 2 attack, 3 dismiss
};

struct PetSpell {
    uint32_t spellId = 0;
    bool autocast = false;
};

/// SMSG_PET_SPELLS for a pet that is out. The packet can end early, and what
/// it did not reach is left as it was.
struct PetSpellsRecord {
    uint64_t guid = 0;
    std::optional<PetMode> mode;
    std::optional<std::array<uint32_t, kActionBarSlots>> actionBar;
    std::optional<std::vector<PetSpell>> spells;
};

/// The pet's own update fields, the ones that arrived in one update.
struct PetFields {
    std::optional<int32_t> attackPower;
    std::optional<float> minDamage;
    std::optional<float> maxDamage;
    std::array<std::optional<int32_t>, 5> stats{};
    /// Armor first, then the six schools.
    std::array<std::optional<int32_t>, 7> resistances{};
    std::optional<uint32_t> experience;
    std::optional<uint32_t> nextLevelExperience;
};

/// A pet kept at a stable master, or the one that is out, as
/// MSG_LIST_STABLED_PETS lists it.
struct StabledPet {
    uint32_t petNumber = 0;   ///< server-side pet number (used for unstable/swap)
    uint32_t entry = 0;       ///< creature template
    uint32_t level = 0;
    std::string name;
    // No display id: MSG_LIST_STABLED_PETS carries only the entry, and a
    // model frame wants a display id. SetPetStablePaperdoll resolves it from
    // `entry` at the moment it draws, so a query still in flight answers on
    // the next read rather than being frozen at zero.
    bool isActive = false;    ///< true = the pet that is out, not stabled
};

} // namespace wowee::pet
