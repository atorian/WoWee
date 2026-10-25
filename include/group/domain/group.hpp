#pragma once

#include "group/domain/events.hpp"
#include "group/domain/records.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wowee::group {

/// The player's party or raid, its pending invite, its ready check and the
/// raid target markers. The server owns the group; every change comes from
/// what it sent, or from the player leaving, and returns what happened.
class Group {
public:
    static constexpr uint8_t kNoMark = 0xFF;

    // ---- What the server sent ----
    std::vector<GroupChange> rosterReceived(Roster roster);
    std::vector<GroupChange> disbanded();
    std::vector<GroupChange> removed();
    std::vector<GroupChange> inviteReceived(const std::string& inviter, bool canAccept);
    /// `playerGuid` is the player's own, which a party's unit tokens skip.
    std::vector<GroupChange> memberStatsReceived(const MemberStats& stats, uint64_t playerGuid);
    std::vector<GroupChange> leaderNamed(const std::string& name);
    /// SMSG_REAL_GROUP_UPDATE: the group's type, its leader and the player's
    /// own member flags.
    std::vector<GroupChange> reshaped(uint8_t groupType, uint8_t playerFlags,
                                      uint64_t leaderGuid, uint64_t playerGuid);
    /// `initiator` is empty when the initiator is not a unit the client knows;
    /// the leader is named then.
    std::vector<GroupChange> readyCheckStarted(const std::string& initiator);
    std::vector<GroupChange> readyCheckAnswered(uint64_t guid, const std::string& name, bool ready);
    std::vector<GroupChange> readyCheckFinished();
    /// A full list replaces every marker; otherwise only the listed icons move.
    std::vector<GroupChange> marksReceived(bool fullList,
                                           const std::vector<std::pair<uint8_t, uint64_t>>& marks);

    // ---- What the player did ----
    std::vector<GroupChange> left();
    void inviteAnswered();
    void readyCheckDismissed() { readyCheckActive_ = false; }
    /// A world transfer: markers belong to the map that was left.
    void forgetMarks() { marks_.fill(0); }
    /// A character is logging in: the last one's group, invite, ready check
    /// and markers are not its own. The server sends the new character's
    /// group with its login.
    void beginCharacter() { *this = Group{}; }
    /// Marks a unit without the server: a solo player's marks, which the
    /// server neither stores nor broadcasts. An icon is unique, and a unit
    /// carries at most one, so placing one clears any previous holder.
    std::vector<GroupChange> markLocally(uint64_t guid, uint8_t icon);

    // ---- Reading it ----
    [[nodiscard]] const Roster& roster() const { return roster_; }
    [[nodiscard]] bool isInGroup() const { return !roster_.isEmpty(); }
    [[nodiscard]] bool isRaid() const { return groupIsRaid(roster_.groupType); }
    [[nodiscard]] bool isLeader(uint64_t playerGuid) const { return roster_.leaderGuid == playerGuid; }
    [[nodiscard]] const Member* member(uint64_t guid) const;
    /// The member holding an assignment, or 0.
    [[nodiscard]] uint64_t assigned(Assignment assignment) const;
    /// The unit token the interface knows a member by: raidN in a raid,
    /// partyN in a party (the player is not counted), or empty.
    [[nodiscard]] std::string unitToken(uint64_t guid, uint64_t playerGuid) const;

    [[nodiscard]] bool hasPendingInvite() const { return invitePending_; }
    [[nodiscard]] const std::string& inviterName() const { return inviter_; }

    [[nodiscard]] bool hasPendingReadyCheck() const { return readyCheckActive_; }
    [[nodiscard]] const std::string& readyCheckInitiator() const { return readyCheckInitiator_; }
    [[nodiscard]] const std::vector<ReadyCheckResult>& readyCheckResults() const { return readyCheckResults_; }

    [[nodiscard]] uint64_t markGuid(uint32_t icon) const { return icon < kRaidMarkCount ? marks_[icon] : 0; }
    /// The icon on a unit, or kNoMark.
    [[nodiscard]] uint8_t markOf(uint64_t guid) const;

private:
    Member* findMember(uint64_t guid);
    void resetReadyCheck();

    Roster roster_;
    bool invitePending_ = false;
    std::string inviter_;
    bool readyCheckActive_ = false;
    uint32_t readyCount_ = 0;
    uint32_t notReadyCount_ = 0;
    std::string readyCheckInitiator_;
    std::vector<ReadyCheckResult> readyCheckResults_;
    std::array<uint64_t, kRaidMarkCount> marks_{};
};

} // namespace wowee::group
