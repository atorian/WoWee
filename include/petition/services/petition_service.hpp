#pragma once

#include "petition/domain/petitions.hpp"
#include "petition/services/petitions.hpp"
#include "petition/services/ports.hpp"

namespace wowee::core { class EventBus; }

namespace wowee::petition {

class PetitionService final : public IPetitions, public IPetitionsFeed {
public:
    PetitionService(IPetitionGateway& gateway, core::EventBus& events);

    // IPetitions: reading
    [[nodiscard]] const PetitionInfo& getPetitionInfo() const override { return petitions_.info(); }
    [[nodiscard]] uint32_t getPetitionCost() const override { return petitions_.cost(); }
    [[nodiscard]] uint64_t getPetitionNpcGuid() const override { return petitions_.vendor(); }
    bool getPetitionCharter(int index, uint32_t& itemId, uint32_t& displayId, uint32_t& cost) const override;
    void closePetitionVendor() override { apply(petitions_.vendorClosed()); }

    // IPetitions: requests
    void buyPetition(uint64_t npcGuid, const std::string& guildName, uint32_t clientIndex = 1) override;
    void signPetition(uint64_t petitionGuid) override;
    void turnInPetition(uint64_t petitionGuid) override;
    void offerPetition(uint64_t petitionGuid, uint64_t targetGuid) override;
    void renamePetition(uint64_t petitionGuid, const std::string& newName) override;

    // IPetitionsFeed
    void onVendor(CharterOffer offer) override { apply(petitions_.vendorShown(std::move(offer))); }
    void onSignatures(uint64_t petitionGuid, uint64_t ownerGuid, uint32_t entry, uint32_t count,
                      std::vector<PetitionSignature> signatures) override;
    void onDetails(uint64_t petitionGuid, std::string name, uint32_t signaturesNeeded, bool arena) override {
        apply(petitions_.detailsReceived(petitionGuid, std::move(name), signaturesNeeded, arena));
    }
    void onSigned(uint64_t petitionGuid, uint64_t playerGuid) override;
    void onSignRefused(SignRefusal reason, uint32_t code) override;
    void onTurnedIn() override;
    void onTurnInRefused(TurnInRefusal reason, uint32_t code) override;
    void onOfferRefused(OfferRefusal reason) override;
    void beginCharacter() override { petitions_.beginCharacter(); }

private:
    void apply(const std::vector<PetitionChange>& changes);

    IPetitionGateway& gateway_;
    core::EventBus& events_;
    Petitions petitions_;
};

} // namespace wowee::petition
