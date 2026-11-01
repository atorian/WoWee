#pragma once

#include "group/domain/group.hpp"
#include "group/services/group.hpp"
#include "group/services/ports.hpp"

#include <string>
#include <vector>

namespace wowee::core { class EventBus; }

namespace wowee::group {

class GroupService final : public IGroup, public IGroupServerFeed {
public:
    GroupService(IGroupGateway& gateway, core::EventBus& events);

    // IGroup: reading
    [[nodiscard]] const Roster& getPartyData() const override { return group_.roster(); }
    [[nodiscard]] bool isInGroup() const override { return group_.isInGroup(); }
    [[nodiscard]] bool hasPendingGroupInvite() const override { return group_.hasPendingInvite(); }
    [[nodiscard]] const std::string& getPendingInviterName() const override { return group_.inviterName(); }
    [[nodiscard]] bool hasPendingReadyCheck() const override { return group_.hasPendingReadyCheck(); }
    [[nodiscard]] const std::string& getReadyCheckInitiator() const override { return group_.readyCheckInitiator(); }
    [[nodiscard]] const std::vector<ReadyCheckResult>& getReadyCheckResults() const override { return group_.readyCheckResults(); }
    [[nodiscard]] uint64_t getRaidMarkGuid(uint32_t icon) const override { return group_.markGuid(icon); }
    [[nodiscard]] uint8_t getEntityRaidMark(uint64_t guid) const override { return group_.markOf(guid); }
    [[nodiscard]] std::string unitTokenOf(uint64_t guid) const override;

    // IGroup: requests
    void inviteToGroup(const std::string& name) override;
    void acceptGroupInvite() override;
    void declineGroupInvite() override;
    void uninvitePlayer(const std::string& name) override;
    void leaveGroup() override;
    void convertToRaid() override;
    void promoteToLeader(uint64_t guid) override;
    void setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) override;
    void setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) override;
    void setMainTank(uint64_t guid) override { assign(Assignment::MainTank, guid); }
    void setMainAssist(uint64_t guid) override { assign(Assignment::MainAssist, guid); }
    void clearMainTank() override { unassign(Assignment::MainTank); }
    void clearMainAssist() override { unassign(Assignment::MainAssist); }
    void setGroupAssistant(uint64_t guid, bool apply) override;
    void setRaidSubgroup(const std::string& name, uint8_t group) override;
    void swapRaidSubgroup(const std::string& first, const std::string& second) override;
    void setRaidMark(uint64_t guid, uint8_t icon) override;
    void initiateReadyCheck() override;
    void respondToReadyCheck(bool ready) override;
    void dismissReadyCheck() override { group_.readyCheckDismissed(); }

    // IGroupServerFeed
    void onRoster(Roster roster) override { apply(group_.rosterReceived(std::move(roster))); }
    void onDisbanded() override { apply(group_.disbanded()); }
    void onRemoved() override { apply(group_.removed()); }
    void onInvite(const std::string& inviter, bool canAccept) override { apply(group_.inviteReceived(inviter, canAccept)); }
    void onInviteDeclined(const std::string& name) override;
    void onInviteCancelled() override;
    void onCommandResult(PartyResult result, const std::string& name) override;
    void onMemberStats(const MemberStats& stats) override {
        apply(group_.memberStatsReceived(stats, gateway_.playerGuid()));
    }
    void onLeaderNamed(const std::string& name) override { apply(group_.leaderNamed(name)); }
    void onReshaped(uint8_t groupType, uint8_t playerFlags, uint64_t leaderGuid) override;
    void onReadyCheckStarted(const std::string& initiator) override { apply(group_.readyCheckStarted(initiator)); }
    void onReadyCheckAnswer(uint64_t guid, const std::string& name, bool ready) override {
        apply(group_.readyCheckAnswered(guid, name, ready));
    }
    void onReadyCheckFinished() override { apply(group_.readyCheckFinished()); }
    void onRaidMarks(bool fullList, const std::vector<std::pair<uint8_t, uint64_t>>& marks) override {
        apply(group_.marksReceived(fullList, marks));
    }
    void onWorldTransfer() override { group_.forgetMarks(); }
    void beginCharacter() override { group_.beginCharacter(); }

private:
    void apply(const std::vector<GroupChange>& changes);
    void assign(Assignment assignment, uint64_t guid);
    void unassign(Assignment assignment);

    IGroupGateway& gateway_;
    core::EventBus& events_;
    Group group_;
};

} // namespace wowee::group
