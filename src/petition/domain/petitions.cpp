#include "petition/domain/petitions.hpp"

#include <utility>

namespace wowee::petition {

std::vector<PetitionChange> Petitions::vendorShown(CharterOffer offer) {
    offer_ = std::move(offer);
    vendorOpen_ = true;
    return {VendorShown{offer_.guildCharter}};
}

std::vector<PetitionChange> Petitions::vendorClosed() {
    if (!vendorOpen_) return {};
    vendorOpen_ = false;
    offer_.npcGuid = 0;
    return {VendorClosed{offer_.guildCharter}};
}

std::vector<PetitionChange> Petitions::signaturesShown(uint64_t petitionGuid, uint64_t ownerGuid, uint32_t count,
                                                       std::vector<PetitionSignature> signatures) {
    info_ = PetitionInfo{};
    info_.petitionGuid = petitionGuid;
    info_.ownerGuid = ownerGuid;
    info_.signatureCount = count;
    info_.signatures = std::move(signatures);
    return {PetitionShown{}};
}

std::vector<PetitionChange> Petitions::detailsReceived(uint64_t petitionGuid, std::string name,
                                                       uint32_t signaturesNeeded, bool arena) {
    if (info_.petitionGuid != petitionGuid) return {};
    info_.guildName = std::move(name);
    if (signaturesNeeded > 0) info_.signaturesRequired = signaturesNeeded;
    info_.isArena = arena;
    return {PetitionShown{}};
}

void Petitions::signatureAdded(uint64_t petitionGuid, uint64_t playerGuid) {
    if (info_.petitionGuid != petitionGuid) return;
    info_.signatureCount++;
    PetitionSignature sig;
    sig.playerGuid = playerGuid;
    info_.signatures.push_back(sig);
}

const Charter* Petitions::charter(int index) const {
    if (index < 0 || index >= static_cast<int>(offer_.charters.size())) return nullptr;
    return &offer_.charters[static_cast<size_t>(index)];
}

} // namespace wowee::petition
