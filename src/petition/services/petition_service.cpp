#include "petition/services/petition_service.hpp"

#include "foundation/event_bus.hpp"

namespace wowee::petition {

PetitionService::PetitionService(IPetitionGateway& gateway, core::EventBus& events)
    : gateway_(gateway), events_(events) {}

void PetitionService::apply(const std::vector<PetitionChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

bool PetitionService::getPetitionCharter(int index, uint32_t& itemId, uint32_t& displayId, uint32_t& cost) const {
    const Charter* c = petitions_.charter(index);
    if (!c) return false;
    itemId = c->itemId;
    displayId = c->displayId;
    cost = c->cost;
    return true;
}

void PetitionService::buyPetition(uint64_t npcGuid, const std::string& guildName, uint32_t clientIndex) {
    if (gateway_.online()) gateway_.sendBuy(npcGuid, guildName, clientIndex);
}

void PetitionService::signPetition(uint64_t petitionGuid) {
    if (gateway_.online()) gateway_.sendSign(petitionGuid);
}

void PetitionService::turnInPetition(uint64_t petitionGuid) {
    if (gateway_.online()) gateway_.sendTurnIn(petitionGuid);
}

void PetitionService::offerPetition(uint64_t petitionGuid, uint64_t targetGuid) {
    if (!gateway_.online() || petitionGuid == 0 || targetGuid == 0) return;
    gateway_.sendOffer(petitionGuid, targetGuid);
}

void PetitionService::renamePetition(uint64_t petitionGuid, const std::string& newName) {
    if (petitionGuid == 0 || newName.empty() || !gateway_.online()) return;
    gateway_.sendRename(petitionGuid, newName);
}

void PetitionService::onSignatures(uint64_t petitionGuid, uint64_t ownerGuid, uint32_t entry, uint32_t count,
                                   std::vector<PetitionSignature> signatures) {
    const auto changes = petitions_.signaturesShown(petitionGuid, ownerGuid, count, std::move(signatures));
    // Everything the frame needs beyond the signatures - the name, the number
    // of signatures required, guild or arena - comes only from the query
    // reply; nothing used to ask, so every charter opened nameless and
    // needing nine.
    if (gateway_.connected()) gateway_.sendQuery(entry, petitionGuid);
    apply(changes);
}

void PetitionService::onSigned(uint64_t petitionGuid, uint64_t playerGuid) {
    events_.publish(PetitionSigned{});
    petitions_.signatureAdded(petitionGuid, playerGuid);
}

void PetitionService::onSignRefused(SignRefusal reason, uint32_t code) {
    events_.publish(SignRefused{reason, code});
}

void PetitionService::onTurnedIn() {
    events_.publish(CharterTurnedIn{});
}

void PetitionService::onTurnInRefused(TurnInRefusal reason, uint32_t code) {
    events_.publish(TurnInRefused{reason, code});
}

void PetitionService::onOfferRefused(OfferRefusal reason) {
    events_.publish(OfferRefused{reason});
}

} // namespace wowee::petition
