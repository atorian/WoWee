#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::petition {

struct PetitionSignature {
    uint64_t playerGuid = 0;
};

/// The charter whose signatures are on screen.
struct PetitionInfo {
    uint64_t petitionGuid = 0;
    uint64_t ownerGuid = 0;
    std::string guildName;
    uint32_t signatureCount = 0;
    /// How many signatures the charter needs. Only SMSG_PETITION_QUERY_RESPONSE
    /// says, so this is the fallback until that reply lands - nine is retail's
    /// guild figure, and AzerothCore's is a config the reply carries.
    uint32_t signaturesRequired = 9;
    /// Guild or arena, from the query reply. The signature frame writes a
    /// different heading and a different confirmation for each, and calling
    /// every charter a guild charter mislabels all three arena ones.
    bool isArena = false;
    std::vector<PetitionSignature> signatures;
};

/// One charter on offer at a registrar.
struct Charter {
    uint32_t itemId = 0;
    uint32_t displayId = 0;
    uint32_t cost = 0;
};

/// What a registrar offers. A guild registrar offers one charter; an arena
/// registrar three, for the two, three and five person teams.
struct CharterOffer {
    uint64_t npcGuid = 0;
    /// The first charter's price, for everything that only wants the one.
    uint32_t cost = 0;
    /// Which of the two panels opens: the offer says, not the opcode.
    bool guildCharter = false;
    std::vector<Charter> charters;
};

/// Why the server refused a signature.
enum class SignRefusal { AlreadySigned, AlreadyInGuild, OwnCharter, Other };
/// Why the server refused to turn a charter in.
enum class TurnInRefusal { AlreadyInGuild, NotEnoughSignatures, NameTaken, Other };
/// Why the server refused to hand a charter to someone.
enum class OfferRefusal { TargetInGuild, TargetHasCharter, Other };

} // namespace wowee::petition
