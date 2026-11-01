#pragma once

#include "group/domain/group_type.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wowee::group {

/// What a member is to the group, as SMSG_GROUP_LIST's flags say.
namespace MemberFlag {
inline constexpr uint8_t Assistant  = 0x01;
inline constexpr uint8_t MainTank   = 0x02;
inline constexpr uint8_t MainAssist = 0x04;
}

/// The two assignments MSG_PARTY_ASSIGNMENT carries.
enum class Assignment : uint8_t { MainTank = 0, MainAssist = 1 };

struct Member {
    std::string name;
    uint64_t guid = 0;
    uint8_t isOnline = 0;      // 0 = offline, 1 = online
    uint8_t subGroup = 0;      // raid subgroup (0 for a party)
    uint8_t flags = 0;         // MemberFlag bits
    uint8_t roles = 0;         // dungeon finder roles (3.3.5a)

    // From SMSG_PARTY_MEMBER_STATS.
    uint32_t curHealth = 0;
    uint32_t maxHealth = 0;
    uint8_t powerType = 0;
    uint16_t curPower = 0;
    uint16_t maxPower = 0;
    uint16_t level = 0;
    uint16_t zoneId = 0;
    int16_t posX = 0;
    int16_t posY = 0;
    uint16_t onlineStatus = 0;   // GROUP_UPDATE_FLAG_STATUS bitmask
    bool hasPartyStats = false;  // true once stats arrived
};

/// The group as SMSG_GROUP_LIST describes it.
struct Roster {
    uint8_t groupType = 0;
    uint8_t subGroup = 0;
    uint8_t flags = 0;
    uint8_t roles = 0;
    uint8_t lootMethod = 0;      ///< 0..4; see lootMethodName
    uint64_t looterGuid = 0;
    uint8_t lootThreshold = 0;
    uint8_t difficultyId = 0;
    uint8_t raidDifficultyId = 0;
    uint32_t memberCount = 0;
    std::vector<Member> members;
    uint64_t leaderGuid = 0;

    [[nodiscard]] bool isValid() const { return true; }
    [[nodiscard]] bool isEmpty() const { return memberCount == 0; }
};

/// SMSG_PARTY_COMMAND_RESULT's result.
enum class PartyResult : uint32_t {
    OK = 0,
    BAD_PLAYER_NAME = 1,
    TARGET_NOT_IN_GROUP = 2,
    TARGET_NOT_IN_INSTANCE = 3,
    GROUP_FULL = 4,
    ALREADY_IN_GROUP = 5,
    NOT_IN_GROUP = 6,
    NOT_LEADER = 7,
    PLAYER_WRONG_FACTION = 8,
    IGNORING_YOU = 9,
    LFG_PENDING = 12,
    INVITE_RESTRICTED = 13
};

/// One member's answer to a ready check.
struct ReadyCheckResult {
    std::string name;
    bool ready = false;
};

/// SMSG_PARTY_MEMBER_STATS, decoded. `present` has a bit for each field the
/// packet actually carried; `updateFlags` is what it announced.
struct MemberStats {
    uint64_t guid = 0;
    uint32_t updateFlags = 0;
    uint32_t present = 0;
    uint16_t onlineStatus = 0;
    uint32_t curHealth = 0;
    uint32_t maxHealth = 0;
    uint8_t powerType = 0;
    uint16_t curPower = 0;
    uint16_t maxPower = 0;
    uint16_t level = 0;
    uint16_t zoneId = 0;
    int16_t posX = 0;
    int16_t posY = 0;
};

/// The bits of MemberStats::updateFlags and present.
namespace StatsField {
inline constexpr uint32_t Status    = 0x0001;
inline constexpr uint32_t CurHealth = 0x0002;
inline constexpr uint32_t MaxHealth = 0x0004;
inline constexpr uint32_t PowerType = 0x0008;
inline constexpr uint32_t CurPower  = 0x0010;
inline constexpr uint32_t MaxPower  = 0x0020;
inline constexpr uint32_t Level     = 0x0040;
inline constexpr uint32_t Zone      = 0x0080;
inline constexpr uint32_t Position  = 0x0100;
inline constexpr uint32_t Auras     = 0x0200;
}

} // namespace wowee::group
