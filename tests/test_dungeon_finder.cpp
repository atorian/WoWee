// The dungeon finder: its domain model, and the service against a recording
// gateway and the real event bus. Only wowee_lfg is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "lfg/services/lfg_service.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::lfg;

namespace {

template <typename E>
bool has(const std::vector<LfgChange>& changes) {
    return std::any_of(changes.begin(), changes.end(),
                       [](const LfgChange& c) { return std::holds_alternative<E>(c); });
}

Proposal proposal(uint32_t state, uint32_t id, bool silent = false) {
    Proposal p;
    p.dungeonId = 48;
    p.state = state;
    p.proposalId = id;
    p.silent = silent;
    return p;
}

QueueUpdate update(uint8_t type, bool extraInfo, bool queued = false, bool inDungeon = false) {
    QueueUpdate u;
    u.updateType = type;
    u.extraInfo = extraInfo;
    u.queued = queued;
    u.inDungeon = inDungeon;
    return u;
}

BootVote vote(bool inProgress, uint32_t votes, uint32_t needed) {
    BootVote v;
    v.inProgress = inProgress;
    v.victimGuid = 9;
    v.bootVotes = votes;
    v.votesNeeded = needed;
    v.totalVotes = 5;
    return v;
}

struct RecordingGateway final : ILfgGateway, ILfgWorld {
    bool world = true, socket = true;
    std::map<uint64_t, std::string> names;
    std::map<uint64_t, std::string> units;
    std::vector<std::string> sent;
    std::vector<uint64_t> asked;

    bool online() const override { return world && socket; }
    bool connected() const override { return socket; }
    void sendJoin(const std::vector<uint32_t>& ids, uint8_t roles) override {
        sent.push_back("join " + std::to_string(ids.size()) + " roles " + std::to_string(roles));
    }
    void sendLeave() override { sent.push_back("leave"); }
    void sendSetRoles(uint8_t roles) override { sent.push_back("roles " + std::to_string(roles)); }
    void sendProposalAnswer(uint32_t id, bool accept) override {
        sent.push_back("answer " + std::to_string(id) + (accept ? " yes" : " no"));
    }
    void sendTeleport(bool in) override { sent.push_back(in ? "teleport in" : "teleport out"); }
    void sendBootVote(bool v) override { sent.push_back(v ? "kick yes" : "kick no"); }
    void sendPlayerLockInfoRequest() override { sent.push_back("locks?"); }
    void sendPartyLockInfoRequest() override { sent.push_back("party locks?"); }

    std::string dungeonName(uint32_t id) const override { return id == 48 ? "Blackfathom Deeps" : ""; }
    std::string playerName(uint64_t guid) const override {
        auto it = names.find(guid);
        return it == names.end() ? std::string() : it->second;
    }
    void requestPlayerName(uint64_t guid) override { asked.push_back(guid); }
    std::optional<std::string> unitName(uint64_t guid) const override {
        auto it = units.find(guid);
        if (it == units.end()) return std::nullopt;
        return it->second;
    }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    LfgService service{gateway, gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<RolesChanged>([this](const auto&) { events.push_back("roles"); }));
        subscriptions.push_back(bus.subscribe<RoleChosen>(
            [this](const RoleChosen& e) { events.push_back("chosen " + e.playerName); }));
        subscriptions.push_back(bus.subscribe<BootUpdated>([this](const auto&) { events.push_back("boot"); }));
    }
};

} // namespace

TEST_CASE("A joined queue takes the server's state; a refusal keeps the old one", "[lfg]") {
    DungeonFinder f;
    auto ok = f.joined(2);
    CHECK(f.state() == LfgState::Queued);
    CHECK(has<JoinedQueue>(ok));
    CHECK(has<QueueUpdated>(ok));
    auto refused = f.joinRefused(JoinRefusal::Deserter);
    CHECK(f.state() == LfgState::Queued);
    CHECK(has<JoinFailed>(refused));
    CHECK(has<QueueUpdated>(refused));
}

TEST_CASE("A new character has none of the last one's dungeon finder", "[lfg]") {
    DungeonFinder f;
    QueueStatus status;
    status.dungeonId = 261;
    status.avgWaitSec = 300;
    f.queueStatusReceived(status);
    Proposal proposal;
    proposal.proposalId = 7;
    proposal.dungeonId = 261;
    f.proposalReceived(proposal);
    f.rolesOffered(0x02);
    f.lockInfoReceived({LfgReward{}}, {{43, 1}});

    f.beginCharacter();

    CHECK(f.state() == LfgState::None);
    CHECK(f.dungeonId() == 0);
    CHECK(f.proposalId() == 0);
    CHECK(f.offeredRoles() == 0);
    CHECK(f.queue().avgWaitSec == -1);
    CHECK(f.locks().empty());
    CHECK(f.rewards().empty());
    // The same proposal arriving for the new character opens its dialog.
    CHECK(has<ProposalShown>(f.proposalReceived(proposal)));
}

TEST_CASE("The queue status puts the player in the queue and keeps its numbers", "[lfg]") {
    DungeonFinder f;
    QueueStatus s;
    s.dungeonId = 48;
    s.myWaitSec = -1;
    s.waitTank = 120;
    s.needDps = 2;
    s.queuedSeconds = 75;
    auto changes = f.queueStatusReceived(s);
    CHECK(f.state() == LfgState::Queued);
    CHECK(f.dungeonId() == 48);
    CHECK(f.queue().waitTank == 120);
    CHECK(f.queue().myWaitSec == -1);
    CHECK(f.queue().queuedSeconds == 75);
    CHECK(has<QueueStatusUpdated>(changes));
    CHECK(has<QueueUpdated>(changes));
}

TEST_CASE("A proposal's dialog opens once, however often it is restated", "[lfg]") {
    DungeonFinder f;
    auto first = f.proposalReceived(proposal(0, 7));
    CHECK(f.state() == LfgState::Proposal);
    CHECK(f.proposalId() == 7);
    CHECK(has<ProposalShown>(first));
    CHECK_FALSE(has<ProposalShown>(f.proposalReceived(proposal(0, 7))));
    CHECK(has<ProposalShown>(f.proposalReceived(proposal(0, 8))));
}

TEST_CASE("A silent proposal opens no dialog", "[lfg]") {
    DungeonFinder f;
    auto changes = f.proposalReceived(proposal(0, 7, true));
    CHECK_FALSE(has<ProposalShown>(changes));
    CHECK(has<ProposalUpdated>(changes));
}

TEST_CASE("A failed proposal goes back to the queue; a successful one into the dungeon", "[lfg]") {
    DungeonFinder f;
    f.proposalReceived(proposal(0, 7));
    auto failed = f.proposalReceived(proposal(1, 7));
    CHECK(f.state() == LfgState::Queued);
    CHECK(f.proposalId() == 0);
    REQUIRE(has<ProposalEnded>(failed));
    CHECK_FALSE(std::get<ProposalEnded>(failed.back()).succeeded);
    // The failed proposal closed its dialog, so the same id may open it again.
    CHECK(has<ProposalShown>(f.proposalReceived(proposal(0, 7))));
    auto done = f.proposalReceived(proposal(2, 7));
    CHECK(f.state() == LfgState::InDungeon);
    CHECK(std::get<ProposalEnded>(done.back()).succeeded);
}

TEST_CASE("The role check popup shows as it begins and hides on any ending", "[lfg]") {
    DungeonFinder f;
    RoleCheck begin;
    begin.state = 2;
    begin.members = 3;
    auto shown = f.roleCheckReceived(begin);
    CHECK(f.state() == LfgState::RoleCheck);
    CHECK(f.roleCheckMembers() == 3);
    CHECK(std::get<RoleCheckPopup>(shown.back()).show);
    RoleCheck missing;
    missing.state = 3;
    auto failed = f.roleCheckReceived(missing);
    CHECK(f.state() == LfgState::None);
    CHECK(has<RoleCheckFailed>(failed));
    CHECK_FALSE(std::get<RoleCheckPopup>(failed.back()).show);
    RoleCheck finished;
    finished.state = 1;
    f.roleCheckReceived(finished);
    CHECK(f.state() == LfgState::Queued);
}

TEST_CASE("The status restatement keeps the player queued", "[lfg]") {
    DungeonFinder f;
    f.queueUpdated(update(5, true, true));
    CHECK(f.state() == LfgState::Queued);
    auto restated = f.queueUpdated(update(15, true, true));
    CHECK(f.state() == LfgState::Queued);
    CHECK_FALSE(has<QueueNews>(restated));
    CHECK_FALSE(has<UnknownQueueUpdate>(restated));
}

TEST_CASE("Queue news is said only when the state moves", "[lfg]") {
    DungeonFinder f;
    CHECK(has<QueueNews>(f.queueUpdated(update(5, true, true))));
    CHECK_FALSE(has<QueueNews>(f.queueUpdated(update(5, true, true))));
    auto removed = f.queueUpdated(update(8, false));
    CHECK(f.state() == LfgState::None);
    CHECK(has<QueueNews>(removed));
}

TEST_CASE("Types that never carry a body say nothing about the queue", "[lfg]") {
    DungeonFinder f;
    f.queueUpdated(update(5, true, true));
    f.queueUpdated(update(16, false));
    CHECK(f.state() == LfgState::Queued);
    f.queueUpdated(update(0, false));
    CHECK(f.state() == LfgState::Queued);
}

TEST_CASE("A found group is a proposal though the queue flag is set", "[lfg]") {
    DungeonFinder f;
    f.queueUpdated(update(11, true, true));
    CHECK(f.state() == LfgState::Proposal);
}

TEST_CASE("A party update says in the dungeon; an unknown type changes nothing", "[lfg]") {
    DungeonFinder f;
    f.queueUpdated(update(12, true, false, true));
    CHECK(f.state() == LfgState::InDungeon);
    auto unknown = f.queueUpdated(update(99, true, true));
    CHECK(has<UnknownQueueUpdate>(unknown));
}

TEST_CASE("An update names the dungeon it is about", "[lfg]") {
    DungeonFinder f;
    QueueUpdate u = update(5, true, true);
    u.dungeonId = 48;
    f.queueUpdated(u);
    CHECK(f.dungeonId() == 48);
    f.queueUpdated(update(15, true, true));
    CHECK(f.dungeonId() == 48);
}

TEST_CASE("A finished run keeps its reward for the alert to read back", "[lfg]") {
    DungeonFinder f;
    LfgCompletionReward r;
    r.money = 500;
    auto changes = f.rewarded(r);
    CHECK(f.completionReward().valid);
    CHECK(f.completionReward().money == 500);
    CHECK(f.state() == LfgState::FinishedDungeon);
    CHECK(has<CompletionRewarded>(changes));
}

TEST_CASE("A vote to kick ends passed or failed and forgets itself", "[lfg]") {
    DungeonFinder f;
    BootVote v = vote(true, 1, 3);
    v.reason = "afk";
    f.bootVoteReceived(v, "Garrosh");
    CHECK(f.state() == LfgState::Boot);
    CHECK(f.boot().inProgress);
    CHECK(f.bootTargetName() == "Garrosh");
    CHECK(f.bootReason() == "afk");
    auto passed = f.bootVoteReceived(vote(false, 3, 3), "Garrosh");
    CHECK(std::get<BootEnded>(passed.front()).passed);
    CHECK(f.state() == LfgState::InDungeon);
    CHECK_FALSE(f.boot().inProgress);
    CHECK(f.bootTargetName().empty());
    CHECK(f.bootReason().empty());
    auto failed = f.bootVoteReceived(vote(false, 2, 3), "");
    CHECK_FALSE(std::get<BootEnded>(failed.front()).passed);
}

TEST_CASE("A vote restated without its reason keeps the reason", "[lfg]") {
    DungeonFinder f;
    BootVote v = vote(true, 1, 3);
    v.reason = "afk";
    f.bootVoteReceived(v, "Garrosh");
    f.bootVoteReceived(vote(true, 2, 3), "Garrosh");
    CHECK(f.bootReason() == "afk");
    CHECK(f.boot().bootVotes == 2);
}

TEST_CASE("Offered roles are kept and announced before they are sent", "[lfg]") {
    Fixture f;
    f.service.lfgSetRoles(0x0A);
    CHECK(f.service.getLfgOfferedRoles() == 0x0A);
    CHECK(f.events == std::vector<std::string>{"roles"});
    CHECK(f.gateway.sent == std::vector<std::string>{"roles 10"});
    f.gateway.world = false;
    f.service.lfgSetRoles(0x02);
    CHECK(f.service.getLfgOfferedRoles() == 0x02);
    CHECK(f.gateway.sent.size() == 1);
}

TEST_CASE("A join needs a dungeon and the world", "[lfg]") {
    Fixture f;
    f.service.lfgJoin({}, 0x08);
    f.gateway.world = false;
    f.service.lfgJoin({48}, 0x08);
    CHECK(f.gateway.sent.empty());
    f.gateway.world = true;
    f.service.lfgJoin({48, 49}, 0x08);
    CHECK(f.gateway.sent == std::vector<std::string>{"join 2 roles 8"});
}

TEST_CASE("Leaving clears the queue state even outside the world", "[lfg]") {
    Fixture f;
    f.service.onQueueStatus(QueueStatus{});
    f.gateway.world = false;
    f.service.lfgLeave();
    CHECK(f.service.getLfgState() == LfgState::None);
    CHECK(f.gateway.sent == std::vector<std::string>{"leave"});
    f.service.onQueueStatus(QueueStatus{});
    f.gateway.socket = false;
    f.service.lfgLeave();
    CHECK(f.service.isLfgQueued());
}

TEST_CASE("Answers, teleports and votes need only a connection", "[lfg]") {
    Fixture f;
    f.gateway.world = false;
    f.service.lfgAcceptProposal(7, true);
    f.service.lfgTeleport(false);
    f.service.lfgSetBootVote(true);
    f.service.requestLfgPlayerLockInfo();
    CHECK(f.gateway.sent == std::vector<std::string>{"answer 7 yes", "teleport out", "kick yes"});
    f.gateway.socket = false;
    f.service.lfgAcceptProposal(7, false);
    CHECK(f.gateway.sent.size() == 3);
}

TEST_CASE("The kick victim is named, and asked for when unknown", "[lfg]") {
    Fixture f;
    f.gateway.names[9] = "Garrosh";
    f.service.onBootVote(vote(true, 1, 3));
    CHECK(f.service.getLfgBootTargetName() == "Garrosh");
    CHECK(f.gateway.asked.empty());
    f.gateway.names.clear();
    f.service.onBootVote(vote(true, 1, 3));
    CHECK(f.gateway.asked == std::vector<uint64_t>{9});
}

TEST_CASE("A role choice names the unit, or a player", "[lfg]") {
    Fixture f;
    f.gateway.units[4] = "Thrall";
    f.service.onRoleChosen(4, true, 0x02);
    f.service.onRoleChosen(5, true, 0x08);
    CHECK(f.events == std::vector<std::string>{"chosen Thrall", "chosen A player"});
}

TEST_CASE("The dungeon being queued for is named from the table", "[lfg]") {
    Fixture f;
    QueueStatus s;
    s.dungeonId = 48;
    f.service.onQueueStatus(s);
    CHECK(f.service.getCurrentLfgDungeonName() == "Blackfathom Deeps");
}
