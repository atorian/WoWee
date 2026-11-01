#pragma once

#include <cstdint>

namespace wowee::reputation {

/// Faction flags (SMSG_INITIALIZE_FACTIONS, Faction.dbc ReputationFlags).
inline constexpr uint8_t kFactionVisible        = 0x01;  // shown in the reputation list
inline constexpr uint8_t kFactionAtWar          = 0x02;
inline constexpr uint8_t kFactionHidden         = 0x04;  // never shown
inline constexpr uint8_t kFactionInvisibleForced = 0x08;
inline constexpr uint8_t kFactionPeaceForced    = 0x10;  // war cannot be declared
inline constexpr uint8_t kFactionInactive       = 0x20;  // moved to the inactive list

/// The player's standing at one reputation index, as SMSG_INITIALIZE_FACTIONS
/// lists it.
struct FactionStanding {
    uint8_t flags = 0;
    int32_t standing = 0;
};

} // namespace wowee::reputation
