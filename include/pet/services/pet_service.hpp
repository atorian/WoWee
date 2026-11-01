#pragma once

#include "pet/domain/pet.hpp"
#include "pet/domain/records.hpp"
#include "pet/services/pet.hpp"
#include "pet/services/ports.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::pet {

class PetService final : public IPet, public IPetFeed {
public:
    PetService(IPetGateway& gateway, core::EventBus& events);

    [[nodiscard]] bool hasPet() const override { return pet_.guid() != 0; }
    [[nodiscard]] uint64_t getPetGuid() const override { return pet_.guid(); }
    [[nodiscard]] uint32_t getPetActionSlot(int index) const override { return pet_.actionSlot(index); }
    [[nodiscard]] uint8_t getPetCommand() const override { return pet_.mode().command; }
    [[nodiscard]] uint8_t getPetReact() const override { return pet_.mode().react; }
    [[nodiscard]] const std::array<int32_t, 5>& getPetStats() const override { return pet_.stats(); }
    [[nodiscard]] const std::array<int32_t, 7>& getPetResistances() const override { return pet_.resistances(); }
    [[nodiscard]] int32_t getPetAttackPower() const override { return pet_.attackPower(); }
    [[nodiscard]] float getPetMinDamage() const override { return pet_.minDamage(); }
    [[nodiscard]] float getPetMaxDamage() const override { return pet_.maxDamage(); }
    [[nodiscard]] uint32_t getPetExperience() const override { return pet_.experience(); }
    [[nodiscard]] uint32_t getPetNextLevelExp() const override { return pet_.nextLevelExperience(); }
    [[nodiscard]] const std::vector<uint32_t>& getPetSpells() const override { return pet_.spells(); }
    [[nodiscard]] bool isPetSpellAutocast(uint32_t spellId) const override { return pet_.isAutocast(spellId); }
    [[nodiscard]] bool isPetRenameable() const override { return pet_.renameable(); }

    void sendPetAction(uint32_t action, uint64_t targetGuid = 0) override;
    void dismissPet() override;
    void togglePetSpellAutocast(uint32_t spellId) override;
    void renamePet(const std::string& newName) override;
    void abandonPet() override;

    [[nodiscard]] bool showPetUnlearnDialog() const override { return pet_.unlearnOffered(); }
    [[nodiscard]] uint32_t getPetUnlearnCost() const override { return pet_.unlearnCost(); }
    void confirmPetUnlearn() override;
    void cancelPetUnlearn() override { pet_.declineUnlearn(); }

    [[nodiscard]] bool isStableWindowOpen() const override { return stable_.isOpen(); }
    void closeStableWindow() override { publish(stable_.close()); }
    [[nodiscard]] uint8_t getStableSlots() const override { return stable_.slots(); }
    [[nodiscard]] const std::vector<StabledPet>& getStabledPets() const override { return stable_.pets(); }
    void requestStabledPetList() override;
    void stablePet(uint8_t slot) override;
    void unstablePet(uint32_t petNumber) override;
    void buyStableSlot() override;

    void onPetSpells(const PetSpellsRecord& record) override { publish(pet_.summoned(record)); }
    void onPetDismissed() override { publish(pet_.dismissed()); }
    void onPetLost() override { publish(pet_.lost()); }
    void onSpellLearned(uint32_t spellId) override { publish(pet_.learned(spellId)); }
    void onSpellUnlearned(uint32_t spellId) override { publish(pet_.unlearned(spellId)); }
    void onMode(uint64_t petGuid, PetMode mode) override { publish(pet_.modeChanged(petGuid, mode)); }
    void onFields(const PetFields& fields) override { publish(pet_.fieldsArrived(fields)); }
    void onRenameable() override { publish(pet_.mayBeRenamed()); }
    void onUnlearnOffered(uint32_t cost) override { pet_.unlearnOffered(cost); }
    void onStableList(uint64_t masterGuid, uint8_t slots, std::vector<StabledPet> pets) override {
        publish(stable_.listed(masterGuid, slots, std::move(pets)));
    }
    void onStableResult(uint8_t result) override;
    void onStableMasterChosen(uint64_t npcGuid) override { stable_.chooseMaster(npcGuid); }
    void beginCharacter() override;

private:
    void publish(const std::vector<PetEvent>& events);

    IPetGateway& gateway_;
    core::EventBus& events_;
    Pet pet_;
    Stable stable_;
};

} // namespace wowee::pet
