#pragma once

#include "petition/domain/records.hpp"

#include <cstdint>
#include <variant>

namespace wowee::petition {

// What happened to the registrar's offer and the charter on screen. Petitions
// returns the first three; the service publishes them and the server's
// answers, and the chat log and the addon layer subscribe.

/// A registrar's panel opened (GUILD_REGISTRAR_SHOW or PETITION_VENDOR_SHOW).
struct VendorShown { bool guild = false; };
/// It closed, naming the same panel that opened.
struct VendorClosed { bool guild = false; };
/// The signature frame opens or redraws on this (PETITION_SHOW).
struct PetitionShown {};

using PetitionChange = std::variant<VendorShown, VendorClosed, PetitionShown>;

// The server's answers to the requests. `code` is the server's own number,
// for a refusal this client has no words for.
struct PetitionSigned {};
struct SignRefused { SignRefusal reason = SignRefusal::Other; uint32_t code = 0; };
struct CharterTurnedIn {};
struct TurnInRefused { TurnInRefusal reason = TurnInRefusal::Other; uint32_t code = 0; };
struct OfferRefused { OfferRefusal reason = OfferRefusal::Other; };

} // namespace wowee::petition
