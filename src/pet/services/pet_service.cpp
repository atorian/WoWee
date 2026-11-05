#include "pet/services/pet_service.hpp"

#include "foundation/event_bus.hpp"
#include "pet/domain/actions.hpp"

namespace wowee::pet {

namespace {

class NoPet final : public IPet {
public:
    bool hasPet() const override { return false; }
    uint64_t getPetGuid() const override { return 0; }
    uint32_t getPetActionSlot(int) const override { return 0; }
    uint8_t getPetCommand() const override { return 1; }
    uint8_t getPetReact() const override { return 1; }
    const std::array<int32_t, 5>& getPetStats() const override { return stats_; }
    const std::array<int32_t, 7>& getPetResistances() const override { return resistances_; }
    int32_t getPetAttackPower() const override { return 0; }
    float getPetMinDamage() const override { return 0.0f; }
    float getPetMaxDamage() const override { return 0.0f; }
    uint32_t getPetExperience() const override { return 0; }
    uint32_t getPetNextLevelExp() const override { return 0; }
    const std::vector<uint32_t>& getPetSpells() const override { return spells_; }
    bool isPetSpellAutocast(uint32_t) const override { return false; }
    bool isPetRenameable() const override { return false; }
    void sendPetAction(uint32_t, uint64_t) override {}
    void dismissPet() override {}
    void togglePetSpellAutocast(uint32_t) override {}
    void renamePet(const std::string&) override {}
    void abandonPet() override {}
    bool showPetUnlearnDialog() const override { return false; }
    uint32_t getPetUnlearnCost() const override { return 0; }
    void confirmPetUnlearn() override {}
    void cancelPetUnlearn() override {}
    bool isStableWindowOpen() const override { return false; }
    void closeStableWindow() override {}
    uint8_t getStableSlots() const override { return 0; }
    const std::vector<StabledPet>& getStabledPets() const override { return stabled_; }
    void requestStabledPetList() override {}
    void stablePet(uint8_t) override {}
    void unstablePet(uint32_t) override {}
    void buyStableSlot() override {}

private:
    std::array<int32_t, 5> stats_{};
    std::array<int32_t, 7> resistances_{};
    std::vector<uint32_t> spells_;
    std::vector<StabledPet> stabled_;
};

NoPet kNoPet;

} // namespace

IPet& orNoPet(IPet* pet) {
    return pet ? *pet : kNoPet;
}

PetService::PetService(IPetGateway& gateway, core::EventBus& events)
    : gateway_(gateway), events_(events) {}

void PetService::sendPetAction(uint32_t action, uint64_t targetGuid) {
    if (!hasPet() || !gateway_.online()) return;
    gateway_.sendAction(pet_.guid(), action, targetGuid);
}

void PetService::dismissPet() {
    if (!hasPet() || !gateway_.online()) return;
    // Dismiss is COMMAND_ABANDON. Packing action 0 here sent COMMAND_STAY, so
    // the pet planted itself instead of leaving.
    gateway_.sendAction(pet_.guid(), packPetAction(ActionType::Command, kAbandon), 0);
}

void PetService::togglePetSpellAutocast(uint32_t spellId) {
    if (!hasPet() || spellId == 0 || !gateway_.online()) return;
    const bool on = !pet_.isAutocast(spellId);
    gateway_.sendAutocast(pet_.guid(), spellId, on);
    pet_.setAutocast(spellId, on);
}

void PetService::renamePet(const std::string& newName) {
    if (!hasPet() || !gateway_.online()) return;
    if (newName.empty() || newName.size() > 12) return;
    gateway_.sendRename(pet_.guid(), newName);
}

// Abandoning is not dismissing. Dismiss puts a hunter's pet away and it can be
// called back; abandon gives it up for good, which is why the interface asks
// first and why this is its own message rather than another pet action.
void PetService::abandonPet() {
    if (!hasPet() || !gateway_.online()) return;
    gateway_.sendAbandon(pet_.guid());
}

void PetService::confirmPetUnlearn() {
    if (!pet_.takeUnlearnOffer()) return;
    if (!gateway_.online()) return;
    gateway_.sendUnlearnTalents();
    events_.publish(PetTalentResetRequested{});
}

void PetService::requestStabledPetList() {
    if (!gateway_.online() || stable_.master() == 0) return;
    gateway_.requestStableList(stable_.master());
}

void PetService::stablePet(uint8_t slot) {
    if (!gateway_.online() || stable_.master() == 0) return;
    if (!hasPet()) {
        events_.publish(NoPetToStable{});
        return;
    }
    gateway_.sendStable(stable_.master(), slot);
}

void PetService::unstablePet(uint32_t petNumber) {
    if (!gateway_.online() || stable_.master() == 0 || petNumber == 0) return;
    gateway_.sendUnstable(stable_.master(), petNumber);
}

// The stable master sells a fourth, fifth and sixth slot; the panel's buy
// button asks for one at a time and the server bills for whichever is next.
// CMSG_BUY_STABLE_SLOT carries nothing but who is being asked.
void PetService::buyStableSlot() {
    if (!gateway_.online() || stable_.master() == 0) return;
    gateway_.sendBuyStableSlot(stable_.master());
}

void PetService::onStableResult(uint8_t result) {
    events_.publish(StableResult{result});
    // Every success changes what the stable holds, and the window shows the
    // list as it last arrived.
    if (stable_.isOpen() && stable_.master() != 0 && gateway_.connected() && result <= 0x08)
        gateway_.requestStableList(stable_.master());
}

void PetService::beginCharacter() {
    pet_.beginCharacter();
    stable_.beginCharacter();
}

void PetService::publish(const std::vector<PetEvent>& events) {
    for (const auto& e : events)
        std::visit([this](const auto& v) { events_.publish(v); }, e);
}

} // namespace wowee::pet
