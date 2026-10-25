#include "group/services/group_service.hpp"

#include "core/event_bus.hpp"

namespace wowee::group {

namespace {

/// The group of a client with no game: never in one, and every request
/// dropped.
class NoGroup final : public IGroup {
public:
    const Roster& getPartyData() const override { return roster_; }
    bool isInGroup() const override { return false; }
    bool hasPendingGroupInvite() const override { return false; }
    const std::string& getPendingInviterName() const override { return empty_; }
    bool hasPendingReadyCheck() const override { return false; }
    const std::string& getReadyCheckInitiator() const override { return empty_; }
    const std::vector<ReadyCheckResult>& getReadyCheckResults() const override { return results_; }
    uint64_t getRaidMarkGuid(uint32_t) const override { return 0; }
    uint8_t getEntityRaidMark(uint64_t) const override { return Group::kNoMark; }
    std::string unitTokenOf(uint64_t) const override { return {}; }
    void inviteToGroup(const std::string&) override {}
    void acceptGroupInvite() override {}
    void declineGroupInvite() override {}
    void uninvitePlayer(const std::string&) override {}
    void leaveGroup() override {}
    void convertToRaid() override {}
    void promoteToLeader(uint64_t) override {}
    void setLootMethod(uint8_t, uint64_t, uint8_t) override {}
    void setPartyAssignment(uint8_t, uint64_t, bool) override {}
    void setMainTank(uint64_t) override {}
    void setMainAssist(uint64_t) override {}
    void clearMainTank() override {}
    void clearMainAssist() override {}
    void setGroupAssistant(uint64_t, bool) override {}
    void setRaidSubgroup(const std::string&, uint8_t) override {}
    void swapRaidSubgroup(const std::string&, const std::string&) override {}
    void setRaidMark(uint64_t, uint8_t) override {}
    void initiateReadyCheck() override {}
    void respondToReadyCheck(bool) override {}
    void dismissReadyCheck() override {}

private:
    Roster roster_;
    std::string empty_;
    std::vector<ReadyCheckResult> results_;
};

NoGroup kNoGroup;

} // namespace

IGroup& orNoGroup(IGroup* group) {
    return group ? *group : kNoGroup;
}

GroupService::GroupService(IGroupGateway& gateway, core::EventBus& events)
    : gateway_(gateway), events_(events) {}

std::string GroupService::unitTokenOf(uint64_t guid) const {
    return group_.unitToken(guid, gateway_.playerGuid());
}

void GroupService::inviteToGroup(const std::string& name) {
    if (gateway_.canSend()) gateway_.sendInvite(name);
}

void GroupService::acceptGroupInvite() {
    if (!gateway_.canSend()) return;
    group_.inviteAnswered();
    gateway_.sendAccept();
}

void GroupService::declineGroupInvite() {
    if (!gateway_.canSend()) return;
    group_.inviteAnswered();
    gateway_.sendDecline();
}

void GroupService::uninvitePlayer(const std::string& name) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(UninviteNeedsName{}); return; }
    gateway_.sendUninvite(name);
    events_.publish(UninviteSent{name});
}

void GroupService::leaveGroup() {
    if (!gateway_.canSend()) return;
    gateway_.sendLeave();
    apply(group_.left());
}

void GroupService::convertToRaid() {
    if (!gateway_.canSend()) return;
    if (!group_.isInGroup()) { events_.publish(NotInGroup{}); return; }
    if (!group_.isLeader(gateway_.playerGuid())) { events_.publish(NotGroupLeader{}); return; }
    if (group_.isRaid()) { events_.publish(AlreadyInRaid{}); return; }
    gateway_.sendConvertToRaid();
}

void GroupService::promoteToLeader(uint64_t guid) {
    // The server checks that the player leads and that the target is a member.
    if (gateway_.canSend() && guid != 0) gateway_.sendLeader(guid);
}

void GroupService::setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) {
    if (!gateway_.canSend()) return;
    constexpr uint8_t kMasterLoot = 2;
    gateway_.sendLootMethod(method, method == kMasterLoot ? masterGuid : 0, threshold);
}

void GroupService::setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) {
    if (gateway_.canSend() && guid != 0) gateway_.sendAssignment(assignment, guid, apply);
}

void GroupService::assign(Assignment assignment, uint64_t guid) {
    if (!gateway_.canSend()) return;
    if (guid == 0) { events_.publish(NoTargetSelected{}); return; }
    gateway_.sendAssignment(static_cast<uint8_t>(assignment), guid, true);
    events_.publish(AssignmentSet{assignment});
}

void GroupService::unassign(Assignment assignment) {
    if (!gateway_.canSend()) return;
    // MSG_PARTY_ASSIGNMENT takes it away from someone, and the group list
    // says who holds it.
    if (const uint64_t holder = group_.assigned(assignment))
        gateway_.sendAssignment(static_cast<uint8_t>(assignment), holder, false);
    events_.publish(AssignmentCleared{assignment});
}

void GroupService::setGroupAssistant(uint64_t guid, bool apply) {
    if (gateway_.canSend() && guid != 0) gateway_.sendAssistant(guid, apply);
}

void GroupService::setRaidSubgroup(const std::string& name, uint8_t group) {
    if (!gateway_.canSend()) return;
    if (name.empty() || group < 1 || group > 8) return;
    gateway_.sendSubgroup(name, static_cast<uint8_t>(group - 1));
}

void GroupService::swapRaidSubgroup(const std::string& first, const std::string& second) {
    if (!gateway_.canSend()) return;
    if (first.empty() || second.empty()) return;
    gateway_.sendSwapSubgroup(first, second);
}

void GroupService::setRaidMark(uint64_t guid, uint8_t icon) {
    if (!gateway_.canSend()) return;
    // Marks are group state on the server, which drops them from an ungrouped
    // player and broadcasts nothing back. A solo player's marks are applied
    // here instead; joining a group is safe, as the server's full list then
    // replaces them.
    if (!group_.isInGroup()) {
        apply(group_.markLocally(guid, icon));
        return;
    }
    if (icon == Group::kNoMark) {
        const uint8_t current = group_.markOf(guid);
        if (current != Group::kNoMark) gateway_.sendRaidMark(current, 0);
    } else if (icon < kRaidMarkCount) {
        gateway_.sendRaidMark(icon, guid);
    }
}

void GroupService::initiateReadyCheck() {
    if (!gateway_.canSend()) return;
    if (!group_.isInGroup()) { events_.publish(ReadyCheckNeedsGroup{}); return; }
    gateway_.sendReadyCheck();
    events_.publish(ReadyCheckSent{});
}

void GroupService::respondToReadyCheck(bool ready) {
    if (!gateway_.canSend()) return;
    gateway_.sendReadyCheckAnswer(ready);
    events_.publish(ReadyCheckResponded{ready});
}

void GroupService::onInviteDeclined(const std::string& name) {
    events_.publish(InviteDeclined{name});
}

void GroupService::onInviteCancelled() {
    events_.publish(InviteCancelled{});
}

void GroupService::onCommandResult(PartyResult result, const std::string& name) {
    if (result != PartyResult::OK) events_.publish(PartyCommandFailed{result, name});
}

void GroupService::onReshaped(uint8_t groupType, uint8_t playerFlags, uint64_t leaderGuid) {
    apply(group_.reshaped(groupType, playerFlags, leaderGuid, gateway_.playerGuid()));
}

void GroupService::apply(const std::vector<GroupChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

} // namespace wowee::group
