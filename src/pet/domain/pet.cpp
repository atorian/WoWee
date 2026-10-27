#include "pet/domain/pet.hpp"

#include <algorithm>

namespace wowee::pet {

namespace {
constexpr uint8_t kCommandAttack = 2;
}

void Pet::clear() {
    guid_ = 0;
    spells_.clear();
    autocast_.clear();
    actionBar_.fill(0);
}

std::vector<PetEvent> Pet::summoned(const PetSpellsRecord& record) {
    if (record.guid == 0) return dismissed();
    guid_ = record.guid;
    if (record.mode) mode_ = *record.mode;
    if (record.actionBar) actionBar_ = *record.actionBar;
    if (record.spells) {
        spells_.clear();
        autocast_.clear();
        for (const auto& s : *record.spells) {
            if (s.spellId == 0) continue;
            spells_.push_back(s.spellId);
            if (s.autocast) autocast_.insert(s.spellId);
        }
    }
    return {PetChanged{true}};
}

std::vector<PetEvent> Pet::dismissed() {
    clear();
    return {PetChanged{false}};
}

std::vector<PetEvent> Pet::lost() {
    clear();
    return {PetLost{}};
}

std::vector<PetEvent> Pet::learned(uint32_t spellId) {
    spells_.push_back(spellId);
    return {PetLearnedSpell{spellId}};
}

std::vector<PetEvent> Pet::unlearned(uint32_t spellId) {
    spells_.erase(std::remove(spells_.begin(), spells_.end(), spellId), spells_.end());
    autocast_.erase(spellId);
    return {PetUnlearnedSpell{spellId}};
}

std::vector<PetEvent> Pet::modeChanged(uint64_t guid, PetMode mode) {
    if (guid != guid_) return {};
    // The pet frame lights its attack indicator on the start and clears it on
    // the stop, so only the crossing matters - the mode arrives for every
    // change, and announcing each would relight an indicator already lit.
    const bool wasAttacking = mode_.command == kCommandAttack;
    mode_ = mode;
    const bool nowAttacking = mode_.command == kCommandAttack;
    if (nowAttacking == wasAttacking) return {};
    if (nowAttacking) return {PetAttackStarted{}};
    return {PetAttackStopped{}};
}

std::vector<PetEvent> Pet::fieldsArrived(const PetFields& fields) {
    bool statsChanged = false;
    bool experienceChanged = false;
    if (fields.attackPower) { attackPower_ = *fields.attackPower; statsChanged = true; }
    if (fields.minDamage) { minDamage_ = *fields.minDamage; statsChanged = true; }
    if (fields.maxDamage) { maxDamage_ = *fields.maxDamage; statsChanged = true; }
    for (size_t i = 0; i < stats_.size(); ++i)
        if (fields.stats[i]) { stats_[i] = *fields.stats[i]; statsChanged = true; }
    for (size_t i = 0; i < resistances_.size(); ++i)
        if (fields.resistances[i]) { resistances_[i] = *fields.resistances[i]; statsChanged = true; }
    if (fields.experience && *fields.experience != experience_) {
        experience_ = *fields.experience;
        experienceChanged = true;
    }
    if (fields.nextLevelExperience && *fields.nextLevelExperience != nextLevelExperience_) {
        nextLevelExperience_ = *fields.nextLevelExperience;
        experienceChanged = true;
    }
    std::vector<PetEvent> events;
    if (experienceChanged) events.emplace_back(PetExperienceChanged{});
    if (statsChanged) events.emplace_back(PetStatsChanged{});
    return events;
}

std::vector<PetEvent> Pet::mayBeRenamed() {
    renameable_ = true;
    return {PetRenameable{}};
}

void Pet::setAutocast(uint32_t spellId, bool on) {
    if (on) autocast_.insert(spellId); else autocast_.erase(spellId);
}

void Pet::unlearnOffered(uint32_t cost) {
    unlearnCost_ = cost;
    unlearnPending_ = true;
}

bool Pet::takeUnlearnOffer() {
    if (!unlearnPending_) return false;
    unlearnPending_ = false;
    unlearnCost_ = 0;
    return true;
}

uint32_t Pet::actionSlot(int index) const {
    if (index < 0 || index >= kActionBarSlots) return 0;
    return actionBar_[static_cast<size_t>(index)];
}

void Stable::chooseMaster(uint64_t npcGuid) {
    master_ = npcGuid;
    open_ = false;
}

std::vector<PetEvent> Stable::listed(uint64_t masterGuid, uint8_t slots, std::vector<StabledPet> pets) {
    master_ = masterGuid;
    slots_ = slots;
    pets_ = std::move(pets);
    open_ = true;
    return {StableShown{}};
}

std::vector<PetEvent> Stable::close() {
    const bool wasOpen = open_;
    open_ = false;
    // The stable window hides on this and nothing else.
    if (!wasOpen) return {};
    return {StableClosed{}};
}

} // namespace wowee::pet
