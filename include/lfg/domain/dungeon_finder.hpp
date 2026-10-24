#pragma once

#include "lfg/domain/events.hpp"
#include "lfg/domain/records.hpp"

#include <unordered_map>
#include <vector>

namespace wowee::lfg {

/// The dungeon finder: where this player is in it, the queue's numbers, the
/// proposal and role check on screen, the vote to kick, and what the finder
/// pays and forbids.
class DungeonFinder {
public:
    /// The server queued the player, in the state it names.
    std::vector<LfgChange> joined(uint32_t state);
    /// The server would not; the state is left as it was.
    std::vector<LfgChange> joinRefused(JoinRefusal reason);
    std::vector<LfgChange> lockInfoReceived(std::vector<LfgReward> rewards,
                                            std::unordered_map<uint32_t, uint32_t> locks);
    std::vector<LfgChange> queueStatusReceived(const QueueStatus& status);
    std::vector<LfgChange> proposalReceived(Proposal proposal);
    std::vector<LfgChange> roleCheckReceived(RoleCheck check);
    std::vector<LfgChange> queueUpdated(const QueueUpdate& update);
    std::vector<LfgChange> rewarded(LfgCompletionReward reward);
    std::vector<LfgChange> bootVoteReceived(BootVote vote, std::string victimName);
    std::vector<LfgChange> rolesOffered(uint8_t roles);
    /// The player left the queue; the server is told, and says nothing back.
    void left() { state_ = LfgState::None; }
    /// A character is logging in: none of the last one's queue, proposal,
    /// role check, vote, rewards or locks is its own.
    void beginCharacter() { *this = DungeonFinder{}; }

    [[nodiscard]] LfgState state() const { return state_; }
    [[nodiscard]] uint32_t dungeonId() const { return dungeonId_; }
    [[nodiscard]] uint32_t proposalId() const { return proposalId_; }
    [[nodiscard]] uint8_t offeredRoles() const { return offeredRoles_; }
    [[nodiscard]] const QueueStatus& queue() const { return queue_; }
    [[nodiscard]] const std::vector<LfgProposalMember>& proposalMembers() const { return proposalMembers_; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint32_t>& locks() const { return locks_; }
    [[nodiscard]] const std::vector<LfgReward>& rewards() const { return rewards_; }
    [[nodiscard]] const LfgCompletionReward& completionReward() const { return completionReward_; }
    [[nodiscard]] const std::vector<LfgRoleCheckDungeon>& roleCheckDungeons() const { return roleCheck_.dungeons; }
    [[nodiscard]] uint8_t roleCheckMembers() const { return roleCheck_.members; }
    [[nodiscard]] const BootVote& boot() const { return boot_; }
    [[nodiscard]] const std::string& bootTargetName() const { return bootTargetName_; }
    [[nodiscard]] const std::string& bootReason() const { return bootReason_; }

private:
    LfgState state_ = LfgState::None;
    uint32_t dungeonId_ = 0;
    uint32_t proposalId_ = 0;
    uint32_t shownProposalId_ = 0;
    /// The roles last offered, which the role-check popup ticks from.
    uint8_t offeredRoles_ = 0;
    QueueStatus queue_;
    std::vector<LfgProposalMember> proposalMembers_;
    std::unordered_map<uint32_t, uint32_t> locks_;
    std::vector<LfgReward> rewards_;
    LfgCompletionReward completionReward_;
    RoleCheck roleCheck_;
    BootVote boot_;
    std::string bootTargetName_;
    std::string bootReason_;
};

} // namespace wowee::lfg
