#pragma once

#include "petition/domain/records.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::petition {

/// Charters as the interface and the addon layer use them.
class IPetitions {
public:
    virtual ~IPetitions() = default;

    // ---- Reading ----
    [[nodiscard]] virtual const PetitionInfo& getPetitionInfo() const = 0;
    [[nodiscard]] virtual uint32_t getPetitionCost() const = 0;
    [[nodiscard]] virtual uint64_t getPetitionNpcGuid() const = 0;
    /// Name, icon and cost of the nth charter a registrar is offering, from
    /// zero. False when it offered no such charter.
    virtual bool getPetitionCharter(int index, uint32_t& itemId, uint32_t& displayId, uint32_t& cost) const = 0;

    /// Shuts the registrar's panel and tells the interface which one.
    virtual void closePetitionVendor() = 0;

    // ---- Requests ----
    virtual void buyPetition(uint64_t npcGuid, const std::string& guildName, uint32_t clientIndex = 1) = 0;
    virtual void signPetition(uint64_t petitionGuid) = 0;
    virtual void turnInPetition(uint64_t petitionGuid) = 0;
    /// Offer the charter to another player so they can sign it.
    virtual void offerPetition(uint64_t petitionGuid, uint64_t targetGuid) = 0;
    /// Rename a charter the player carries; it is an item, named by its guid.
    virtual void renamePetition(uint64_t petitionGuid, const std::string& newName) = 0;
};

/// What the protocol layer tells the charters.
class IPetitionsFeed {
public:
    virtual ~IPetitionsFeed() = default;

    /// SMSG_PETITION_SHOWLIST.
    virtual void onVendor(CharterOffer offer) = 0;
    /// SMSG_PETITION_SHOW_SIGNATURES. `entry` is the charter's item, which the
    /// query for its details needs.
    virtual void onSignatures(uint64_t petitionGuid, uint64_t ownerGuid, uint32_t entry, uint32_t count,
                              std::vector<PetitionSignature> signatures) = 0;
    /// SMSG_PETITION_QUERY_RESPONSE.
    virtual void onDetails(uint64_t petitionGuid, std::string name, uint32_t signaturesNeeded, bool arena) = 0;
    /// SMSG_PETITION_SIGN_RESULTS, signed.
    virtual void onSigned(uint64_t petitionGuid, uint64_t playerGuid) = 0;
    /// SMSG_PETITION_SIGN_RESULTS, refused; `code` is the server's number.
    virtual void onSignRefused(SignRefusal reason, uint32_t code) = 0;
    /// SMSG_TURN_IN_PETITION_RESULTS.
    virtual void onTurnedIn() = 0;
    virtual void onTurnInRefused(TurnInRefusal reason, uint32_t code) = 0;
    /// SMSG_OFFER_PETITION_ERROR.
    virtual void onOfferRefused(OfferRefusal reason) = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::petition
