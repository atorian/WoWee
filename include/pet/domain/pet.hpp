#pragma once

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "pet/domain/events.hpp"
#include "pet/domain/records.hpp"

namespace wowee::pet {

/// The player's pet: which one is out, its bar, its stance, its spells and
/// what its update fields say about it.
class Pet {
public:
    std::vector<PetEvent> summoned(const PetSpellsRecord& record);
    /// SMSG_PET_SPELLS with no pet in it.
    std::vector<PetEvent> dismissed();
    /// SMSG_PET_BROKEN.
    std::vector<PetEvent> lost();
    std::vector<PetEvent> learned(uint32_t spellId);
    std::vector<PetEvent> unlearned(uint32_t spellId);
    /// SMSG_PET_MODE, which names a pet: another's is ignored.
    std::vector<PetEvent> modeChanged(uint64_t guid, PetMode mode);
    std::vector<PetEvent> fieldsArrived(const PetFields& fields);
    std::vector<PetEvent> mayBeRenamed();
    void setAutocast(uint32_t spellId, bool on);
    /// SMSG_PET_UNLEARN_CONFIRM: the server offers to reset the pet's
    /// talents for this much copper.
    void unlearnOffered(uint32_t cost);
    /// The player answered the offer. True when there was one to answer.
    bool takeUnlearnOffer();
    void declineUnlearn() { unlearnPending_ = false; }
    /// A character is logging in: the last one's pet is not this one's.
    void beginCharacter() { *this = Pet{}; }

    [[nodiscard]] uint64_t guid() const { return guid_; }
    [[nodiscard]] uint32_t actionSlot(int index) const;
    [[nodiscard]] const PetMode& mode() const { return mode_; }
    [[nodiscard]] const std::vector<uint32_t>& spells() const { return spells_; }
    [[nodiscard]] bool isAutocast(uint32_t spellId) const { return autocast_.count(spellId) != 0; }
    [[nodiscard]] const std::array<int32_t, 5>& stats() const { return stats_; }
    [[nodiscard]] const std::array<int32_t, 7>& resistances() const { return resistances_; }
    [[nodiscard]] int32_t attackPower() const { return attackPower_; }
    [[nodiscard]] float minDamage() const { return minDamage_; }
    [[nodiscard]] float maxDamage() const { return maxDamage_; }
    [[nodiscard]] uint32_t experience() const { return experience_; }
    [[nodiscard]] uint32_t nextLevelExperience() const { return nextLevelExperience_; }
    [[nodiscard]] bool renameable() const { return renameable_; }
    [[nodiscard]] bool unlearnOffered() const { return unlearnPending_; }
    [[nodiscard]] uint32_t unlearnCost() const { return unlearnCost_; }

private:
    void clear();

    uint64_t guid_ = 0;
    std::array<uint32_t, kActionBarSlots> actionBar_{};
    PetMode mode_;
    std::vector<uint32_t> spells_;
    std::unordered_set<uint32_t> autocast_;
    std::array<int32_t, 5> stats_{};
    std::array<int32_t, 7> resistances_{};
    int32_t attackPower_ = 0;
    float minDamage_ = 0.0f;
    float maxDamage_ = 0.0f;
    uint32_t experience_ = 0;
    uint32_t nextLevelExperience_ = 0;
    bool renameable_ = false;
    bool unlearnPending_ = false;
    uint32_t unlearnCost_ = 0;
};

/// The stable: which stable master it is, how many slots, and the pets in it.
class Stable {
public:
    /// The player chose a stable master's gossip option; the list is asked for
    /// and the window opens when it arrives.
    void chooseMaster(uint64_t npcGuid);
    std::vector<PetEvent> listed(uint64_t masterGuid, uint8_t slots, std::vector<StabledPet> pets);
    std::vector<PetEvent> close();
    /// A character is logging in: it is at no stable master and has no list.
    void beginCharacter() { *this = Stable{}; }

    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] uint64_t master() const { return master_; }
    [[nodiscard]] uint8_t slots() const { return slots_; }
    [[nodiscard]] const std::vector<StabledPet>& pets() const { return pets_; }

private:
    bool open_ = false;
    uint64_t master_ = 0;
    uint8_t slots_ = 0;
    std::vector<StabledPet> pets_;
};

} // namespace wowee::pet
