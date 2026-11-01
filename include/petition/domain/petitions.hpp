#pragma once

#include "petition/domain/events.hpp"
#include "petition/domain/records.hpp"

#include <string>
#include <vector>

namespace wowee::petition {

/// The registrar the player is at, and the charter whose signatures are shown.
class Petitions {
public:
    std::vector<PetitionChange> vendorShown(CharterOffer offer);
    /// Nothing when no panel is open.
    std::vector<PetitionChange> vendorClosed();
    /// `count` is the count the server states, which a short packet may not
    /// list in full.
    std::vector<PetitionChange> signaturesShown(uint64_t petitionGuid, uint64_t ownerGuid, uint32_t count,
                                                std::vector<PetitionSignature> signatures);
    /// What the charter is, from the query reply - for the charter shown only.
    /// Said again, because the frame drew when the signatures arrived.
    std::vector<PetitionChange> detailsReceived(uint64_t petitionGuid, std::string name,
                                                uint32_t signaturesNeeded, bool arena);
    /// Someone's signature went onto a charter.
    void signatureAdded(uint64_t petitionGuid, uint64_t playerGuid);
    /// A character is logging in: the last one's registrar and charter are
    /// not its own.
    void beginCharacter() { *this = Petitions{}; }

    [[nodiscard]] const PetitionInfo& info() const { return info_; }
    [[nodiscard]] uint32_t cost() const { return offer_.cost; }
    [[nodiscard]] uint64_t vendor() const { return offer_.npcGuid; }
    /// The nth charter on offer, from zero, or null.
    [[nodiscard]] const Charter* charter(int index) const;

private:
    bool vendorOpen_ = false;
    CharterOffer offer_;
    PetitionInfo info_;
};

} // namespace wowee::petition
