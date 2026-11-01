#pragma once

#include "lfg/domain/records.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::lfg {

/// The dungeon finder as the interface and the addon layer use it.
class ILfg {
public:
    virtual ~ILfg() = default;

    // ---- State ----
    [[nodiscard]] virtual LfgState getLfgState() const = 0;
    [[nodiscard]] virtual bool isLfgQueued() const = 0;
    [[nodiscard]] virtual bool isLfgInDungeon() const = 0;
    [[nodiscard]] virtual uint32_t getLfgDungeonId() const = 0;
    [[nodiscard]] virtual std::string getCurrentLfgDungeonName() const = 0;
    [[nodiscard]] virtual uint8_t getLfgOfferedRoles() const = 0;
    [[nodiscard]] virtual const std::unordered_map<uint32_t, uint32_t>& getLfgLocks() const = 0;
    [[nodiscard]] virtual const std::vector<LfgReward>& getLfgRewards() const = 0;
    /// What the last finished dungeon-finder run paid out.
    [[nodiscard]] virtual const LfgCompletionReward& getLfgCompletionReward() const = 0;

    // ---- The queue ----
    [[nodiscard]] virtual int32_t getLfgAvgWaitSec() const = 0;
    /// This player's own estimate, in seconds, or -1 while the server has none.
    [[nodiscard]] virtual int32_t getLfgMyWaitSec() const = 0;
    /// How long this player has been in the queue, in seconds.
    [[nodiscard]] virtual uint32_t getLfgQueuedSeconds() const = 0;
    [[nodiscard]] virtual int32_t getLfgWaitTank() const = 0;
    [[nodiscard]] virtual int32_t getLfgWaitHealer() const = 0;
    [[nodiscard]] virtual int32_t getLfgWaitDps() const = 0;
    [[nodiscard]] virtual uint8_t getLfgNeedTank() const = 0;
    [[nodiscard]] virtual uint8_t getLfgNeedHealer() const = 0;
    [[nodiscard]] virtual uint8_t getLfgNeedDps() const = 0;

    // ---- The proposal and the role check ----
    [[nodiscard]] virtual uint32_t getLfgProposalId() const = 0;
    [[nodiscard]] virtual const std::vector<LfgProposalMember>& getLfgProposalMembers() const = 0;
    [[nodiscard]] virtual const std::vector<LfgRoleCheckDungeon>& getLfgRoleCheckDungeons() const = 0;
    [[nodiscard]] virtual uint8_t getLfgRoleCheckMembers() const = 0;

    // ---- The vote to kick ----
    [[nodiscard]] virtual bool isLfgBootInProgress() const = 0;
    [[nodiscard]] virtual bool hasLfgBootVoted() const = 0;
    [[nodiscard]] virtual bool getLfgBootMyVote() const = 0;
    [[nodiscard]] virtual uint32_t getLfgBootVotes() const = 0;
    [[nodiscard]] virtual uint32_t getLfgBootTotal() const = 0;
    [[nodiscard]] virtual uint32_t getLfgBootTimeLeft() const = 0;
    [[nodiscard]] virtual const std::string& getLfgBootTargetName() const = 0;
    [[nodiscard]] virtual const std::string& getLfgBootReason() const = 0;

    // ---- Requests ----
    /// Queue for every dungeon the player ticked; the server queues for all
    /// of them at once. Roles: 0x02 tank, 0x04 healer, 0x08 damage.
    virtual void lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) = 0;
    virtual void lfgLeave() = 0;
    virtual void lfgSetRoles(uint8_t roles) = 0;
    virtual void lfgAcceptProposal(uint32_t proposalId, bool accept) = 0;
    virtual void lfgSetBootVote(bool vote) = 0;
    virtual void lfgTeleport(bool toLfgDungeon = true) = 0;
    /// Ask again which dungeons are locked and what they pay; the ready
    /// popup asks for both as it opens.
    virtual void requestLfgPlayerLockInfo() = 0;
    virtual void requestLfgPartyLockInfo() = 0;
};

/// What the protocol layer tells the dungeon finder.
class ILfgFeed {
public:
    virtual ~ILfgFeed() = default;

    /// SMSG_LFG_JOIN_RESULT: queued, in the state the server names...
    virtual void onJoined(uint32_t state) = 0;
    /// ...or refused.
    virtual void onJoinRefused(JoinRefusal reason) = 0;
    /// SMSG_LFG_PLAYER_INFO: what the random dungeons pay, and what is locked.
    virtual void onLockInfo(std::vector<LfgReward> rewards, std::unordered_map<uint32_t, uint32_t> locks) = 0;
    virtual void onQueueStatus(const QueueStatus& status) = 0;
    virtual void onProposal(Proposal proposal) = 0;
    virtual void onRoleCheck(RoleCheck check) = 0;
    virtual void onQueueUpdate(const QueueUpdate& update) = 0;
    virtual void onReward(LfgCompletionReward reward) = 0;
    virtual void onBootVote(BootVote vote) = 0;
    virtual void onTeleportDenied(TeleportRefusal reason) = 0;
    virtual void onDisabled() = 0;
    /// SMSG_LFG_OFFER_CONTINUE: the dungeon may be resumed.
    virtual void onContinueOffered(uint32_t dungeonId, uint32_t typeId) = 0;
    virtual void onRoleChosen(uint64_t playerGuid, bool ready, uint32_t roles) = 0;
    /// A character is logging in.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::lfg
