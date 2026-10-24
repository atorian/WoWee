#include "lfg/domain/dungeon_finder.hpp"

#include <utility>

namespace wowee::lfg {

namespace {

// 3.3.5 LfgProposalState. Nought is the proposal arriving, not failing.
enum : uint32_t {
    kProposalInitiating = 0,
    kProposalFailed     = 1,
    kProposalSuccess    = 2,
};

// LFG_ROLECHECK_* as the server sends them.
enum : uint32_t {
    kRoleCheckFinished  = 1,
    kRoleCheckBeginning = 2,
    kRoleCheckMissingRole = 3,
};

// 3.3.5 LfgUpdateType, as the server writes it.
enum : uint8_t {
    kLfgDefault             = 0,
    kLfgLeaderUnk1          = 1,
    kLfgRoleCheckAborted    = 4,
    // Five, observed: it arrives the moment a queue is joined. The listings of
    // this enum disagree about whether the join is 5 or 6, so both count; the
    // flags in the body decide the state either way.
    kLfgJoinQueue           = 5,
    kLfgJoinQueueAlt        = 6,
    kLfgRoleCheckFailed     = 7,
    kLfgRemovedFromQueue    = 8,
    kLfgProposalFailed      = 9,
    kLfgProposalDeclined    = 10,
    kLfgGroupFound          = 11,
    kLfgAddedToQueue        = 13,
    kLfgProposalBegin       = 14,
    kLfgUpdateStatus        = 15,
    kLfgGroupMemberOffline  = 16,
    kLfgGroupDisband        = 17,
};

bool newsworthy(uint8_t updateType) {
    switch (updateType) {
        case kLfgJoinQueue:
        case kLfgJoinQueueAlt:
        case kLfgAddedToQueue:
        case kLfgRemovedFromQueue:
        case kLfgRoleCheckFailed:
        case kLfgRoleCheckAborted:
        case kLfgProposalFailed:
        case kLfgProposalDeclined:
        case kLfgGroupFound:
        case kLfgGroupDisband:
            return true;
        default:
            return false;
    }
}

} // namespace

// Into a queue or back out of the attempt: either way the panel re-reads.
std::vector<LfgChange> DungeonFinder::joined(uint32_t state) {
    state_ = static_cast<LfgState>(state);
    return {JoinedQueue{dungeonId_}, QueueUpdated{}};
}

std::vector<LfgChange> DungeonFinder::joinRefused(JoinRefusal reason) {
    return {JoinFailed{reason}, QueueUpdated{}};
}

std::vector<LfgChange> DungeonFinder::lockInfoReceived(std::vector<LfgReward> rewards,
                                                       std::unordered_map<uint32_t, uint32_t> locks) {
    rewards_ = std::move(rewards);
    locks_ = std::move(locks);
    return {LockInfoReceived{}};
}

std::vector<LfgChange> DungeonFinder::queueStatusReceived(const QueueStatus& status) {
    queue_ = status;
    dungeonId_ = status.dungeonId;
    state_ = LfgState::Queued;
    // LFG_UPDATE as well: the minimap's dungeon eye is reached from it alone,
    // and on a login already in the queue this packet is what says so.
    return {QueueStatusUpdated{}, QueueUpdated{}};
}

std::vector<LfgChange> DungeonFinder::proposalReceived(Proposal proposal) {
    proposalMembers_ = std::move(proposal.members);
    dungeonId_ = proposal.dungeonId;
    proposalId_ = proposal.proposalId;

    std::vector<LfgChange> changes;
    switch (proposal.state) {
        case kProposalInitiating:
            state_ = LfgState::Proposal;
            changes.emplace_back(ProposalOffered{proposal.dungeonId});
            break;
        case kProposalFailed:
            state_ = LfgState::Queued;
            proposalId_ = 0;
            changes.emplace_back(ProposalFailed{});
            break;
        case kProposalSuccess:
            state_ = LfgState::InDungeon;
            proposalId_ = 0;
            changes.emplace_back(ProposalSucceeded{proposal.dungeonId});
            break;
        default:
            break;
    }
    changes.emplace_back(ProposalUpdated{});
    switch (proposal.state) {
        case kProposalInitiating:
            if (!proposal.silent && shownProposalId_ != proposal.proposalId) {
                shownProposalId_ = proposal.proposalId;
                changes.emplace_back(ProposalShown{});
            }
            break;
        case kProposalFailed:
            shownProposalId_ = 0;
            changes.emplace_back(ProposalEnded{false});
            break;
        case kProposalSuccess:
            shownProposalId_ = 0;
            changes.emplace_back(ProposalEnded{true});
            break;
        default:
            break;
    }
    return changes;
}

std::vector<LfgChange> DungeonFinder::roleCheckReceived(RoleCheck check) {
    const uint32_t state = check.state;
    roleCheck_ = std::move(check);
    std::vector<LfgChange> changes;
    if (state == kRoleCheckFinished) {
        state_ = LfgState::Queued;
    } else if (state == kRoleCheckMissingRole) {
        state_ = LfgState::None;
        changes.emplace_back(RoleCheckFailed{});
    } else if (state == kRoleCheckBeginning) {
        state_ = LfgState::RoleCheck;
        changes.emplace_back(RoleCheckStarted{});
    }
    changes.emplace_back(RoleCheckUpdated{});
    // Everything but the beginning is an ending: finished, missing a role,
    // the wrong roles between them, aborted, or someone picking none.
    changes.emplace_back(RoleCheckPopup{state == kRoleCheckBeginning});
    return changes;
}

std::vector<LfgChange> DungeonFinder::queueUpdated(const QueueUpdate& update) {
    if (update.dungeonId) dungeonId_ = *update.dungeonId;
    const LfgState previous = state_;

    // A proposal is the one state the flags do not distinguish: the server
    // sends it with the queue flag still set.
    if (update.updateType == kLfgGroupFound || update.updateType == kLfgProposalBegin) {
        state_ = LfgState::Proposal;
    } else if (!update.extraInfo) {
        // No body means none - except for the types that never carry one,
        // which say nothing about the queue at all.
        switch (update.updateType) {
            case kLfgDefault:
            case kLfgLeaderUnk1:
            case kLfgGroupMemberOffline:
                break;
            default:
                state_ = LfgState::None;
                break;
        }
    } else if (update.queued) {
        state_ = LfgState::Queued;
    } else if (update.inDungeon) {
        state_ = LfgState::InDungeon;
    }

    std::vector<LfgChange> changes;
    if (newsworthy(update.updateType)) {
        if (state_ != previous) changes.emplace_back(QueueNews{update.updateType});
    } else if (update.updateType != kLfgUpdateStatus && update.updateType != kLfgDefault) {
        changes.emplace_back(UnknownQueueUpdate{update.updateType, update.extraInfo, update.queued});
    }
    changes.emplace_back(QueueUpdated{});
    return changes;
}

std::vector<LfgChange> DungeonFinder::rewarded(LfgCompletionReward reward) {
    completionReward_ = std::move(reward);
    completionReward_.valid = true;
    state_ = LfgState::FinishedDungeon;
    // The toast carries no arguments and reads the reward back, so the store
    // comes first.
    return {CompletionRewarded{}};
}

std::vector<LfgChange> DungeonFinder::bootVoteReceived(BootVote vote, std::string victimName) {
    std::vector<LfgChange> changes;
    if (vote.reason) bootReason_ = *vote.reason;
    if (vote.inProgress) {
        boot_ = std::move(vote);
        bootTargetName_ = std::move(victimName);
        state_ = LfgState::Boot;
    } else {
        const bool passed = vote.bootVotes >= vote.votesNeeded;
        boot_ = BootVote{};
        bootTargetName_.clear();
        bootReason_.clear();
        state_ = LfgState::InDungeon;
        changes.emplace_back(BootEnded{passed});
    }
    // The vote-to-kick dialog, whose countdown starts on this.
    changes.emplace_back(BootUpdated{});
    return changes;
}

std::vector<LfgChange> DungeonFinder::rolesOffered(uint8_t roles) {
    offeredRoles_ = roles;
    return {RolesChanged{}};
}

} // namespace wowee::lfg
