#pragma once

#include "group/domain/records.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace wowee::group {

// What happened to the player's group. Group returns the GroupChange ones;
// the service publishes them and the outcomes of the player's requests, and
// the chat log, the addon layer and sound subscribe.

/// A new group list arrived (GROUP_ROSTER_UPDATE and its companions).
struct RosterReplaced { bool raid = false; };
struct JoinedGroup { std::vector<std::string> members; };
/// A group list said the player is in no group any more.
struct NoLongerInGroup {};
struct LootMethodChanged { uint8_t method = 0; };
/// The player left on their own; the server does not answer that.
struct LeftGroup {};
struct GroupDisbanded {};
struct RemovedFromGroup {};

struct InviteReceived { std::string inviter; };
/// Invited while already in a group: a refusal, not an offer.
struct InviteBlockedAlreadyGrouped { std::string inviter; };
struct InviteDeclined { std::string name; };
struct InviteCancelled {};
struct PartyCommandFailed { PartyResult result = PartyResult::OK; std::string name; };

struct MemberStatsChanged {
    uint64_t guid = 0;
    /// The unit token the interface knows the member by (partyN, raidN), or
    /// empty for the player themself.
    std::string unit;
    uint32_t updateFlags = 0;
    bool onlineChanged = false;
    bool online = false;
};
struct LeaderChanged { std::string name; };
/// The group's type, leader or the player's own flags changed without a
/// new list (SMSG_REAL_GROUP_UPDATE).
struct GroupReshaped {};

struct ReadyCheckStarted { std::string initiator; };
struct ReadyCheckAnswered { uint64_t guid = 0; std::string name; bool ready = false; };
struct ReadyCheckFinished { uint32_t ready = 0; uint32_t notReady = 0; };

struct RaidMarksChanged {};

// What became of a request the player made, so the chat log reports it
// whichever way the request was made.
struct UninviteSent         { std::string name; };
struct AssignmentSet        { Assignment assignment = Assignment::MainTank; };
struct AssignmentCleared    { Assignment assignment = Assignment::MainTank; };
struct ReadyCheckSent {};
struct ReadyCheckResponded  { bool ready = false; };
// Refused before anything was sent.
struct UninviteNeedsName {};
struct NotInGroup {};
struct NotGroupLeader {};
struct AlreadyInRaid {};
struct NoTargetSelected {};
struct ReadyCheckNeedsGroup {};

using GroupChange = std::variant<
    RosterReplaced, JoinedGroup, NoLongerInGroup, LootMethodChanged, LeftGroup,
    GroupDisbanded, RemovedFromGroup, InviteReceived, InviteBlockedAlreadyGrouped,
    InviteDeclined, InviteCancelled, PartyCommandFailed, MemberStatsChanged,
    LeaderChanged, GroupReshaped, ReadyCheckStarted, ReadyCheckAnswered, ReadyCheckFinished,
    RaidMarksChanged>;

} // namespace wowee::group
