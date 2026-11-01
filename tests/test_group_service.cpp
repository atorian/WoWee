// The group service against a recording gateway and the real event bus.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "group/services/group_service.hpp"

#include <string>
#include <vector>

using namespace wowee::group;

namespace {

struct RecordingGateway final : IGroupGateway {
    bool inWorld = true;
    std::vector<std::string> sent;

    bool canSend() const override { return inWorld; }
    uint64_t playerGuid() const override { return 1; }
    void sendInvite(const std::string& n) override { sent.push_back("invite " + n); }
    void sendAccept() override { sent.push_back("accept"); }
    void sendDecline() override { sent.push_back("decline"); }
    void sendUninvite(const std::string& n) override { sent.push_back("uninvite " + n); }
    void sendLeave() override { sent.push_back("leave"); }
    void sendConvertToRaid() override { sent.push_back("raid"); }
    void sendLeader(uint64_t g) override { sent.push_back("leader " + std::to_string(g)); }
    void sendLootMethod(uint32_t m, uint64_t master, uint32_t t) override {
        sent.push_back("loot " + std::to_string(m) + " " + std::to_string(master) + " " + std::to_string(t));
    }
    void sendAssignment(uint8_t a, uint64_t g, bool apply) override {
        sent.push_back("assign " + std::to_string(a) + " " + std::to_string(g) + (apply ? " on" : " off"));
    }
    void sendAssistant(uint64_t g, bool) override { sent.push_back("assistant " + std::to_string(g)); }
    void sendSubgroup(const std::string& n, uint8_t grp) override { sent.push_back("subgroup " + n + " " + std::to_string(grp)); }
    void sendSwapSubgroup(const std::string&, const std::string&) override { sent.push_back("swap"); }
    void sendRaidMark(uint8_t icon, uint64_t g) override { sent.push_back("mark " + std::to_string(icon) + " " + std::to_string(g)); }
    void sendReadyCheck() override { sent.push_back("readycheck"); }
    void sendReadyCheckAnswer(bool r) override { sent.push_back(r ? "ready" : "notready"); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    GroupService service{gateway, bus};
    std::vector<std::string> notices;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<NotGroupLeader>([this](const auto&) { notices.push_back("not leader"); }));
        subscriptions.push_back(bus.subscribe<AlreadyInRaid>([this](const auto&) { notices.push_back("already raid"); }));
        subscriptions.push_back(bus.subscribe<NotInGroup>([this](const auto&) { notices.push_back("not in group"); }));
        subscriptions.push_back(bus.subscribe<ReadyCheckNeedsGroup>([this](const auto&) { notices.push_back("needs group"); }));
        subscriptions.push_back(bus.subscribe<LeftGroup>([this](const auto&) { notices.push_back("left"); }));
        subscriptions.push_back(bus.subscribe<RaidMarksChanged>([this](const auto&) { notices.push_back("marks"); }));
    }

    void inGroup(uint8_t type, uint64_t leader, std::vector<Member> extra = {}) {
        Roster r;
        r.groupType = type;
        r.leaderGuid = leader;
        Member me;
        me.guid = 1;
        me.name = "Me";
        r.members.push_back(me);
        for (auto& m : extra) r.members.push_back(m);
        r.memberCount = static_cast<uint32_t>(r.members.size());
        service.onRoster(r);
        gateway.sent.clear();
        notices.clear();
    }
};

Member tank(uint64_t guid) {
    Member m;
    m.guid = guid;
    m.name = "Garrosh";
    m.flags = MemberFlag::MainTank;
    return m;
}

} // namespace

TEST_CASE("Main tank is a party assignment, not a raid marker", "[group]") {
    Fixture f;
    f.inGroup(0x02, 1);
    f.service.setMainTank(42);
    f.service.setMainAssist(43);
    CHECK(f.gateway.sent == std::vector<std::string>{"assign 0 42 on", "assign 1 43 on"});
}

TEST_CASE("Clearing the main tank takes the assignment from whoever holds it", "[group]") {
    Fixture f;
    f.inGroup(0x02, 1, {tank(7)});
    f.service.clearMainTank();
    CHECK(f.gateway.sent == std::vector<std::string>{"assign 0 7 off"});
}

TEST_CASE("The master looter is sent only with master loot", "[group]") {
    Fixture f;
    f.service.setLootMethod(2, 7, 3);
    f.service.setLootMethod(3, 7, 2);
    CHECK(f.gateway.sent == std::vector<std::string>{"loot 2 7 3", "loot 3 0 2"});
}

TEST_CASE("Only the leader of a party converts it to a raid", "[group]") {
    Fixture f;
    f.service.convertToRaid();
    CHECK(f.notices == std::vector<std::string>{"not in group"});
    f.inGroup(0x00, 2);
    f.service.convertToRaid();
    CHECK(f.gateway.sent.empty());
    CHECK(f.notices == std::vector<std::string>{"not leader"});
    f.inGroup(0x02, 1);
    f.service.convertToRaid();
    CHECK(f.gateway.sent.empty());
    CHECK(f.notices == std::vector<std::string>{"already raid"});

    f.inGroup(0x00, 1);
    f.service.convertToRaid();
    CHECK(f.gateway.sent == std::vector<std::string>{"raid"});
}

TEST_CASE("A solo mark stays local, a grouped one goes to the server", "[group]") {
    Fixture f;
    f.service.setRaidMark(10, 4);
    CHECK(f.gateway.sent.empty());
    CHECK(f.service.getRaidMarkGuid(4) == 10);
    CHECK(f.notices == std::vector<std::string>{"marks"});

    f.inGroup(0x00, 1);
    f.service.onRaidMarks(true, {{6, 10}});
    f.gateway.sent.clear();
    f.service.setRaidMark(10, 0xFF);
    f.service.setRaidMark(11, 2);
    CHECK(f.gateway.sent == std::vector<std::string>{"mark 6 0", "mark 2 11"});
}

TEST_CASE("Leaving takes effect at once", "[group]") {
    Fixture f;
    f.inGroup(0x00, 1);
    f.service.leaveGroup();
    CHECK(f.gateway.sent == std::vector<std::string>{"leave"});
    CHECK_FALSE(f.service.isInGroup());
    CHECK(f.notices == std::vector<std::string>{"left"});
}

TEST_CASE("Raid groups are counted from one in the panel and from zero on the wire", "[group]") {
    Fixture f;
    f.service.setRaidSubgroup("Thrall", 1);
    f.service.setRaidSubgroup("Thrall", 8);
    f.service.setRaidSubgroup("Thrall", 0);
    f.service.setRaidSubgroup("Thrall", 9);
    CHECK(f.gateway.sent == std::vector<std::string>{"subgroup Thrall 0", "subgroup Thrall 7"});
}

TEST_CASE("A ready check needs a group, and nothing goes out of the world", "[group]") {
    Fixture f;
    f.service.initiateReadyCheck();
    CHECK(f.notices == std::vector<std::string>{"needs group"});
    f.inGroup(0x00, 1);
    f.service.initiateReadyCheck();
    CHECK(f.gateway.sent == std::vector<std::string>{"readycheck"});

    f.gateway.inWorld = false;
    f.gateway.sent.clear();
    f.service.inviteToGroup("Thrall");
    f.service.setMainTank(42);
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("The next character starts with no group, invite, ready check or marks", "[group]") {
    Fixture f;
    f.inGroup(0x02, 1, {tank(7)});
    f.service.onInvite("Thrall", true);
    f.service.onReadyCheckStarted("");
    f.service.onRaidMarks(true, {{2, 7}});

    f.service.beginCharacter();

    CHECK_FALSE(f.service.isInGroup());
    CHECK_FALSE(f.service.hasPendingGroupInvite());
    CHECK_FALSE(f.service.hasPendingReadyCheck());
    CHECK(f.service.getRaidMarkGuid(2) == 0);
    // Nothing of the last raid is left to lead: the next one needs a group again.
    f.service.initiateReadyCheck();
    CHECK(f.gateway.sent.empty());
}
