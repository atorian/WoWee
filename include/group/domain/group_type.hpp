#pragma once

#include <cstdint>

namespace wowee::group {

/// Whether a group's type says raid.
///
/// The field changed meaning between expansions and the client tested it as a
/// number. 1.12 writes 0 for a party and 1 for a raid; 3.3.5 writes a set of
/// flags - 0x01 battleground, 0x02 raid, 0x04 and 0x08 for the dungeon finder -
/// so `== 1` read a WotLK raid as no raid at all, and every test for a party
/// written as `== 0` read a finder group (0x0C) as no party either. A dungeon
/// group had neither kind of frame.
///
/// The low two bits cover both conventions: 1.12's raid is 1, WotLK's is 2, and
/// a battleground group is a raid as well. The finder's bits mean neither.
inline bool groupIsRaid(uint8_t groupType) {
    return (groupType & 0x03) != 0;
}

/// How many raid target markers there are: the icons the server indexes. The
/// one count of them; everything else names this.
inline constexpr uint32_t kRaidMarkCount = 8;

} // namespace wowee::group
