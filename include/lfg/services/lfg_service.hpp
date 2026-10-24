#pragma once

#include "lfg/domain/dungeon_finder.hpp"
#include "lfg/services/lfg.hpp"
#include "lfg/services/ports.hpp"

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::lfg {

class LfgService final : public ILfg, public ILfgFeed {
public:
    LfgService(ILfgGateway& gateway, ILfgWorld& world, core::EventBus& events);

    // ILfg: state
    [[nodiscard]] LfgState getLfgState() const override { return finder_.state(); }
    [[nodiscard]] bool isLfgQueued() const override { return finder_.state() == LfgState::Queued; }
    [[nodiscard]] bool isLfgInDungeon() const override { return finder_.state() == LfgState::InDungeon; }
    [[nodiscard]] uint32_t getLfgDungeonId() const override { return finder_.dungeonId(); }
    [[nodiscard]] std::string getCurrentLfgDungeonName() const override { return world_.dungeonName(finder_.dungeonId()); }
    [[nodiscard]] uint8_t getLfgOfferedRoles() const override { return finder_.offeredRoles(); }
    [[nodiscard]] const std::unordered_map<uint32_t, uint32_t>& getLfgLocks() const override { return finder_.locks(); }
    [[nodiscard]] const std::vector<LfgReward>& getLfgRewards() const override { return finder_.rewards(); }
    [[nodiscard]] const LfgCompletionReward& getLfgCompletionReward() const override { return finder_.completionReward(); }

    // ILfg: the queue
    [[nodiscard]] int32_t getLfgAvgWaitSec() const override { return finder_.queue().avgWaitSec; }
    [[nodiscard]] int32_t getLfgMyWaitSec() const override { return finder_.queue().myWaitSec; }
    [[nodiscard]] uint32_t getLfgQueuedSeconds() const override { return finder_.queue().queuedSeconds; }
    [[nodiscard]] int32_t getLfgWaitTank() const override { return finder_.queue().waitTank; }
    [[nodiscard]] int32_t getLfgWaitHealer() const override { return finder_.queue().waitHealer; }
    [[nodiscard]] int32_t getLfgWaitDps() const override { return finder_.queue().waitDps; }
    [[nodiscard]] uint8_t getLfgNeedTank() const override { return finder_.queue().needTank; }
    [[nodiscard]] uint8_t getLfgNeedHealer() const override { return finder_.queue().needHealer; }
    [[nodiscard]] uint8_t getLfgNeedDps() const override { return finder_.queue().needDps; }

    // ILfg: the proposal and the role check
    [[nodiscard]] uint32_t getLfgProposalId() const override { return finder_.proposalId(); }
    [[nodiscard]] const std::vector<LfgProposalMember>& getLfgProposalMembers() const override { return finder_.proposalMembers(); }
    [[nodiscard]] const std::vector<LfgRoleCheckDungeon>& getLfgRoleCheckDungeons() const override { return finder_.roleCheckDungeons(); }
    [[nodiscard]] uint8_t getLfgRoleCheckMembers() const override { return finder_.roleCheckMembers(); }

    // ILfg: the vote to kick
    [[nodiscard]] bool isLfgBootInProgress() const override { return finder_.boot().inProgress; }
    [[nodiscard]] bool hasLfgBootVoted() const override { return finder_.boot().didVote; }
    [[nodiscard]] bool getLfgBootMyVote() const override { return finder_.boot().myVote; }
    [[nodiscard]] uint32_t getLfgBootVotes() const override { return finder_.boot().bootVotes; }
    [[nodiscard]] uint32_t getLfgBootTotal() const override { return finder_.boot().totalVotes; }
    [[nodiscard]] uint32_t getLfgBootTimeLeft() const override { return finder_.boot().timeLeft; }
    [[nodiscard]] const std::string& getLfgBootTargetName() const override { return finder_.bootTargetName(); }
    [[nodiscard]] const std::string& getLfgBootReason() const override { return finder_.bootReason(); }

    // ILfg: requests
    void lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) override;
    void lfgLeave() override;
    void lfgSetRoles(uint8_t roles) override;
    void lfgAcceptProposal(uint32_t proposalId, bool accept) override;
    void lfgSetBootVote(bool vote) override;
    void lfgTeleport(bool toLfgDungeon = true) override;
    void requestLfgPlayerLockInfo() override;
    void requestLfgPartyLockInfo() override;

    // ILfgFeed
    void onJoined(uint32_t state) override { apply(finder_.joined(state)); }
    void onJoinRefused(JoinRefusal reason) override { apply(finder_.joinRefused(reason)); }
    void onLockInfo(std::vector<LfgReward> rewards, std::unordered_map<uint32_t, uint32_t> locks) override {
        apply(finder_.lockInfoReceived(std::move(rewards), std::move(locks)));
    }
    void onQueueStatus(const QueueStatus& status) override { apply(finder_.queueStatusReceived(status)); }
    void onProposal(Proposal proposal) override { apply(finder_.proposalReceived(std::move(proposal))); }
    void onRoleCheck(RoleCheck check) override { apply(finder_.roleCheckReceived(std::move(check))); }
    void onQueueUpdate(const QueueUpdate& update) override { apply(finder_.queueUpdated(update)); }
    void onReward(LfgCompletionReward reward) override { apply(finder_.rewarded(std::move(reward))); }
    void onBootVote(BootVote vote) override;
    void onTeleportDenied(TeleportRefusal reason) override;
    void onDisabled() override;
    void onContinueOffered(uint32_t dungeonId, uint32_t typeId) override;
    void onRoleChosen(uint64_t playerGuid, bool ready, uint32_t roles) override;
    void beginCharacter() override { finder_.beginCharacter(); }

private:
    void apply(const std::vector<LfgChange>& changes);

    ILfgGateway& gateway_;
    ILfgWorld& world_;
    core::EventBus& events_;
    DungeonFinder finder_;
};

} // namespace wowee::lfg
