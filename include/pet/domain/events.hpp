#pragma once

#include <cstdint>
#include <variant>

namespace wowee::pet {

/// The pet came or went, or its bar was replaced (UNIT_PET, PET_UI_UPDATE,
/// and PET_BAR_UPDATE when `barChanged`).
struct PetChanged { bool barChanged = false; };
/// The pet died or its bond broke.
struct PetLost {};
struct PetLearnedSpell { uint32_t spellId = 0; };
/// The bar drew the spell; it is not the pet's any more.
struct PetUnlearnedSpell { uint32_t spellId = 0; };
/// The pet's command crossed into or out of attacking.
struct PetAttackStarted {};
struct PetAttackStopped {};
struct PetExperienceChanged {};
/// Any of the pet's stats, damage or resistances arrived.
struct PetStatsChanged {};
/// The pet may be named (first tame).
struct PetRenameable {};
/// The stable list arrived, which both opens the stable and refreshes it.
struct StableShown {};
struct StableClosed {};

using PetEvent = std::variant<PetChanged, PetLost, PetLearnedSpell, PetUnlearnedSpell, PetAttackStarted,
                              PetAttackStopped, PetExperienceChanged, PetStatsChanged, PetRenameable,
                              StableShown, StableClosed>;

// ---- Published by the service ----

/// SMSG_STABLE_RESULT.
struct StableResult { uint8_t result = 0; };
/// A pet was to be stabled, and the player has none out.
struct NoPetToStable {};
/// CMSG_PET_UNLEARN_TALENTS went to the server.
struct PetTalentResetRequested {};

} // namespace wowee::pet
