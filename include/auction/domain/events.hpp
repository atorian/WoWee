#pragma once

#include "auction/domain/records.hpp"

#include <cstdint>
#include <variant>

namespace wowee::auction {

// What happened at the auction house.
struct AuctionHouseOpened {};
struct AuctionHouseClosed {};
struct ListChanged { AuctionList list = AuctionList::Browse; };
/// `action` is SMSG_AUCTION_COMMAND_RESULT's: 0 sell, 1 cancel, 2 bid or buyout.
struct ActionSucceeded { uint32_t action = 0; };
struct ActionFailed { uint32_t action = 0; uint32_t error = 0; };

using AuctionChange = std::variant<AuctionHouseOpened, AuctionHouseClosed, ListChanged,
                                   ActionSucceeded, ActionFailed>;

} // namespace wowee::auction
