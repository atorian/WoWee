#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::pet {

struct PetSpellsRecord;
struct PetFields;
struct PetMode;
struct StabledPet;

/// The player's pet and the stable, as the interface, the addon layer and the
/// rest of the game use them.
class IPet {
public:
    virtual ~IPet() = default;

    [[nodiscard]] virtual bool hasPet() const = 0;
    [[nodiscard]] virtual uint64_t getPetGuid() const = 0;
    /// A packed action from the pet bar, or zero.
    [[nodiscard]] virtual uint32_t getPetActionSlot(int index) const = 0;
    [[nodiscard]] virtual uint8_t getPetCommand() const = 0;  ///< 0 stay, 1 follow, 2 attack, 3 dismiss
    [[nodiscard]] virtual uint8_t getPetReact() const = 0;    ///< 0 passive, 1 defensive, 2 aggressive
    /// Strength, agility, stamina, intellect and spirit.
    [[nodiscard]] virtual const std::array<int32_t, 5>& getPetStats() const = 0;
    /// Armor, then the six schools.
    [[nodiscard]] virtual const std::array<int32_t, 7>& getPetResistances() const = 0;
    [[nodiscard]] virtual int32_t getPetAttackPower() const = 0;
    [[nodiscard]] virtual float getPetMinDamage() const = 0;
    [[nodiscard]] virtual float getPetMaxDamage() const = 0;
    [[nodiscard]] virtual uint32_t getPetExperience() const = 0;
    [[nodiscard]] virtual uint32_t getPetNextLevelExp() const = 0;
    [[nodiscard]] virtual const std::vector<uint32_t>& getPetSpells() const = 0;
    [[nodiscard]] virtual bool isPetSpellAutocast(uint32_t spellId) const = 0;
    /// Since SMSG_PET_RENAMEABLE.
    [[nodiscard]] virtual bool isPetRenameable() const = 0;

    /// CMSG_PET_ACTION with a packed action; see pet/domain/actions.hpp.
    virtual void sendPetAction(uint32_t action, uint64_t targetGuid = 0) = 0;
    /// Put the pet away (COMMAND_ABANDON as a pet action - a hunter calls it back).
    virtual void dismissPet() = 0;
    virtual void togglePetSpellAutocast(uint32_t spellId) = 0;
    virtual void renamePet(const std::string& newName) = 0;
    /// Give a hunter's pet up for good. The interface asks first.
    virtual void abandonPet() = 0;

    // ---- Pet talents ----
    /// The server offered to reset the pet's talents, and the player has not
    /// answered yet.
    [[nodiscard]] virtual bool showPetUnlearnDialog() const = 0;
    [[nodiscard]] virtual uint32_t getPetUnlearnCost() const = 0;
    virtual void confirmPetUnlearn() = 0;
    virtual void cancelPetUnlearn() = 0;

    // ---- The stable ----
    [[nodiscard]] virtual bool isStableWindowOpen() const = 0;
    virtual void closeStableWindow() = 0;
    [[nodiscard]] virtual uint8_t getStableSlots() const = 0;
    [[nodiscard]] virtual const std::vector<StabledPet>& getStabledPets() const = 0;
    /// The player is talking to a stable master; the window opens when the
    /// list it asks for arrives.
    virtual void setStableMaster(uint64_t npcGuid) = 0;
    virtual void requestStabledPetList() = 0;
    virtual void stablePet(uint8_t slot) = 0;
    virtual void unstablePet(uint32_t petNumber) = 0;
    /// Buy the next stable slot from the stable master currently open.
    virtual void buyStableSlot() = 0;
};

/// `pet`, or that of a client with no game: no pet, no stable, and every
/// request dropped - for a reader that would otherwise test for it at every
/// call.
[[nodiscard]] IPet& orNoPet(IPet* pet);

/// What the protocol layer tells the pet.
class IPetFeed {
public:
    virtual ~IPetFeed() = default;

    /// SMSG_PET_SPELLS naming a pet.
    virtual void onPetSpells(const PetSpellsRecord& record) = 0;
    /// SMSG_PET_SPELLS with no pet.
    virtual void onPetDismissed() = 0;
    /// SMSG_PET_BROKEN.
    virtual void onPetLost() = 0;
    virtual void onSpellLearned(uint32_t spellId) = 0;
    virtual void onSpellUnlearned(uint32_t spellId) = 0;
    /// SMSG_PET_MODE.
    virtual void onMode(uint64_t petGuid, PetMode mode) = 0;
    /// The pet's own update fields from one update.
    virtual void onFields(const PetFields& fields) = 0;
    /// SMSG_PET_RENAMEABLE.
    virtual void onRenameable() = 0;
    /// SMSG_PET_UNLEARN_CONFIRM.
    virtual void onUnlearnOffered(uint32_t cost) = 0;
    /// MSG_LIST_STABLED_PETS.
    virtual void onStableList(uint64_t masterGuid, uint8_t slots, std::vector<StabledPet> pets) = 0;
    /// SMSG_STABLE_RESULT.
    virtual void onStableResult(uint8_t result) = 0;
    /// A character is logging in: the last one's pet and stable are not this
    /// one's.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::pet
