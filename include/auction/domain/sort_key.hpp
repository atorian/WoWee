#pragma once

#include <cstdint>

namespace wowee::auction {

/// One column of a search's ordering, as the wire carries it.
///
/// `column` is AzerothCore's AuctionSortOrder - MINLEVEL 0, RARITY 1, BUYOUT 2,
/// TIMELEFT 3, ITEM 5, MINBIDBUY 6, OWNER 7, BID 8, STACK 9.
///
/// `descending` goes across as sent. The server's comparator is not uniformly
/// ordered - it answers -1 for the *higher* required level and -1 for the
/// *lower* quality - and the original interface's per-column reverse flags were
/// written against those inversions. Normalising the flag here would undo that
/// pairing and sort half the columns backwards.
struct SortKey {
    uint8_t column = 0;
    bool descending = false;
};

} // namespace wowee::auction
