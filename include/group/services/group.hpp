#pragma once

#include "group/domain/records.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wowee::group {

/// The player's group as the interface and the addon layer use it.
class IGroup {
public:
    virtual ~IGroup() = default;

    // ---- Reading ----
    [[nodiscard]] virtual const Roster& getPartyData() const = 0;
    [[nodiscard]] virtual bool isInGroup() const = 0;
    [[nodiscard]] virtual bool hasPendingGroupInvite() const = 0;
    [[nodiscard]] virtual const std::string& getPendingInviterName() const = 0;
    [[nodiscard]] virtual bool hasPendingReadyCheck() const = 0;
    [[nodiscard]] virtual const std::string& getReadyCheckInitiator() const = 0;
    [[nodiscard]] virtual const std::vector<ReadyCheckResult>& getReadyCheckResults() const = 0;
    /// The unit on a raid target marker, or 0.
    [[nodiscard]] virtual uint64_t getRaidMarkGuid(uint32_t icon) const = 0;
    /// The marker on a unit, or 0xFF.
    [[nodiscard]] virtual uint8_t getEntityRaidMark(uint64_t guid) const = 0;
    /// The unit token a member is known by (raidN, partyN), or empty.
    [[nodiscard]] virtual std::string unitTokenOf(uint64_t guid) const = 0;

    // ---- Requests ----
    virtual void inviteToGroup(const std::string& name) = 0;
    virtual void acceptGroupInvite() = 0;
    virtual void declineGroupInvite() = 0;
    virtual void uninvitePlayer(const std::string& name) = 0;
    /// Leaves at once: the server does not answer a leave.
    virtual void leaveGroup() = 0;
    virtual void convertToRaid() = 0;
    virtual void promoteToLeader(uint64_t guid) = 0;
    /// The master looter is sent only for master loot (method 2).
    virtual void setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) = 0;
    /// MSG_PARTY_ASSIGNMENT: 0 main tank, 1 main assist.
    virtual void setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) = 0;
    virtual void setMainTank(uint64_t guid) = 0;
    virtual void setMainAssist(uint64_t guid) = 0;
    virtual void clearMainTank() = 0;
    virtual void clearMainAssist() = 0;
    virtual void setGroupAssistant(uint64_t guid, bool apply) = 0;
    /// Moves a raid member into a group of eight, counted from one as the
    /// raid panel counts them.
    virtual void setRaidSubgroup(const std::string& name, uint8_t group) = 0;
    /// Exchanges two members, for a destination group that is full.
    virtual void swapRaidSubgroup(const std::string& first, const std::string& second) = 0;
    /// Icon 0..7, or 0xFF to clear the unit's marker.
    virtual void setRaidMark(uint64_t guid, uint8_t icon) = 0;
    virtual void initiateReadyCheck() = 0;
    virtual void respondToReadyCheck(bool ready) = 0;
    virtual void dismissReadyCheck() = 0;
};

/// `group`, or a group the player is never in that sends nothing when there is
/// none - for a reader that would otherwise test for it at every call.
[[nodiscard]] IGroup& orNoGroup(IGroup* group);

/// What the protocol layer tells the group, decoded from whichever
/// expansion's packets.
class IGroupServerFeed {
public:
    virtual ~IGroupServerFeed() = default;

    virtual void onRoster(Roster roster) = 0;
    virtual void onDisbanded() = 0;
    virtual void onRemoved() = 0;
    virtual void onInvite(const std::string& inviter, bool canAccept) = 0;
    virtual void onInviteDeclined(const std::string& name) = 0;
    virtual void onInviteCancelled() = 0;
    virtual void onCommandResult(PartyResult result, const std::string& name) = 0;
    virtual void onMemberStats(const MemberStats& stats) = 0;
    virtual void onLeaderNamed(const std::string& name) = 0;
    virtual void onReshaped(uint8_t groupType, uint8_t playerFlags, uint64_t leaderGuid) = 0;
    virtual void onReadyCheckStarted(const std::string& initiator) = 0;
    virtual void onReadyCheckAnswer(uint64_t guid, const std::string& name, bool ready) = 0;
    virtual void onReadyCheckFinished() = 0;
    virtual void onRaidMarks(bool fullList, const std::vector<std::pair<uint8_t, uint64_t>>& marks) = 0;
    virtual void onWorldTransfer() = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::group
