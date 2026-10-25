#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "group/domain/group_type.hpp"
#include "group/domain/records.hpp"

namespace wowee {
namespace game {

// The group's records are the group domain's (group/domain/records.hpp); these
// are their protocol names.
using GroupMember = group::Member;
using GroupListData = group::Roster;
using PartyResult = group::PartyResult;
using group::groupIsRaid;
using group::kRaidMarkCount;

/// What a loot method is called, as it is written to chat.
///
/// Five, not the three the field's comment used to name. The party window and
/// the /loot command each wrote the five out; a parallel table of lowercase
/// tokens lives in lua_api_helpers.hpp for parsing the command's argument, and
/// the two are in the same order on purpose - the token's index is the wire
/// value this names.
inline const char* lootMethodName(uint8_t method) {
    static constexpr const char* kByMethod[] = {
        "Free for All", "Round Robin", "Master Looter",
        "Group Loot", "Need Before Greed",
    };
    return method < 5 ? kByMethod[method] : "Unknown";
}

/// What an instance difficulty is called, or null for a value that is not one.
///
/// Three places wrote the four words out, and they disagreed about the answer
/// for a value outside the four: two said "Normal" and one said "Unknown".
/// Null here so each keeps its own - the Lua bindings hand this to FrameXML,
/// which compares it against names, while the minimap is only labelling a
/// corner and can say it does not know.
inline const char* instanceDifficultyName(uint32_t difficulty) {
    static constexpr const char* kByDifficulty[] = {
        "Normal", "Heroic", "25 Normal", "25 Heroic",
    };
    return difficulty < 4 ? kByDifficulty[difficulty] : nullptr;
}

/// What a raid target marker is called, or null for a value that is not one.
///
/// Written out capitalised for the minimap's tooltip and lowercase for what
/// /mark matches, plus a third time inside that command's help line - one set
/// of eight words under two spellings. The display spelling lives here and
/// the command lowercases what it compares against it.
inline const char* raidMarkName(uint8_t mark) {
    static constexpr const char* kByMark[] = {
        "Star", "Circle", "Diamond", "Triangle",
        "Moon", "Square", "Cross", "Skull",
    };
    return mark < kRaidMarkCount ? kByMark[mark] : nullptr;
}

/**
 * Party command types
 */
enum class PartyCommand : uint32_t {
    INVITE = 0,
    UNINVITE = 1,
    LEAVE = 2,
    // 3 is not an operation. The server's PARTY_OP_SWAP is 4, and SWAP was
    // declared 3, so the operation field of SMSG_PARTY_COMMAND_RESULT could
    // never be recognised as a swap.
    SWAP = 4
};

} // namespace game
} // namespace wowee
