// Battlegrounds: the queues, the list, the scoreboard and the positions, and
// the service against a recording gateway and the real event bus. Only
// wowee_battleground is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "battleground/services/battleground_service.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::battleground;

namespace {

using Clock = Battlegrounds::Clock;

template <typename E>
const E* find(const std::vector<BattlegroundChange>& changes) {
    for (const auto& c : changes)
        if (const auto* e = std::get_if<E>(&c)) return e;
    return nullptr;
}

BattlefieldStatus status(uint32_t slot, uint32_t statusId, uint32_t bgTypeId = 2) {
    BattlefieldStatus s;
    s.queueSlot = slot;
    s.statusId = statusId;
    s.bgTypeId = bgTypeId;
    return s;
}

struct RecordingGateway final : IBattlegroundGateway, IBattlegroundWorld {
    bool world = true, battleground = true;
    std::map<uint64_t, std::string> names;
    std::vector<std::string> sent;

    bool online() const override { return world; }
    void sendPort(uint8_t, uint32_t bgTypeId, bool accept) override {
        sent.push_back(std::string(accept ? "enter " : "refuse ") + std::to_string(bgTypeId));
    }
    void sendJoin(uint64_t, uint32_t bgTypeId, uint32_t, bool) override { sent.push_back("join " + std::to_string(bgTypeId)); }
    void sendListRequest(uint32_t bgTypeId) override { sent.push_back("list " + std::to_string(bgTypeId)); }
    void sendReportAfk(uint64_t guid) override { sent.push_back("afk " + std::to_string(guid)); }
    void sendLeave() override { sent.push_back("leave"); }
    void sendScoreRequest() override { sent.push_back("score"); }
    void sendPositionsRequest() override { sent.push_back("positions"); }

    std::string playerName(uint64_t guid) const override {
        auto it = names.find(guid);
        return it == names.end() ? std::string() : it->second;
    }
    bool inBattleground() const override { return battleground; }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    BattlegroundService service{gateway, gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    template <typename E, typename F>
    void record(F describe) {
        subscriptions.push_back(bus.subscribe<E>([this, describe](const E& e) { events.push_back(describe(e)); }));
    }

    Fixture() {
        record<NoInvitation>([](const auto&) { return std::string("none"); });
        record<QueueSlotUpdated>([](const QueueSlotUpdated& e) { return "slot " + std::to_string(e.slot); });
        record<QueueDeclined>([](const QueueDeclined& e) { return e.wasInvite ? std::string("declined invite") : "left " + e.bgName; });
        record<JoinUnanswered>([](const JoinUnanswered& e) { return "unanswered " + std::to_string(e.bgTypeId); });
        record<JoinRefused>([](const JoinRefused& e) { return "refused " + std::to_string(e.code) + " " + e.member; });
        record<GroupQueued>([](const GroupQueued& e) { return "queued " + std::to_string(e.bgTypeId); });
        record<PlayerEntered>([](const PlayerEntered& e) { return "entered " + e.name; });
    }
};

} // namespace

TEST_CASE("A battleground is named by its type, an arena by its size", "[battleground]") {
    Battlegrounds b;
    b.statusReceived(status(0, 1, 2), Clock::now());
    CHECK(b.queues()[0].bgName == "Warsong Gulch");
    BattlefieldStatus arena = status(1, 1, 4);
    arena.arenaType = 2;
    b.statusReceived(arena, Clock::now());
    CHECK(b.queues()[1].bgName == "2v2 Arena (Nagrand Arena)");
    b.statusReceived(status(2, 1, 99), Clock::now());
    CHECK(b.queues()[2].bgName == "Battleground #99");
}

TEST_CASE("A queue's line is said only when it moves", "[battleground]") {
    Battlegrounds b;
    CHECK(find<QueueStatusChanged>(b.statusReceived(status(0, 1), Clock::now())));
    auto again = b.statusReceived(status(0, 1), Clock::now());
    CHECK_FALSE(find<QueueStatusChanged>(again));
    REQUIRE(find<QueueSlotUpdated>(again));
    CHECK(find<QueueSlotUpdated>(again)->slot == 1);
    CHECK(find<QueueStatusChanged>(b.statusReceived(status(0, 2), Clock::now())));
}

TEST_CASE("The queue keeps its wait times in seconds", "[battleground]") {
    Battlegrounds b;
    BattlefieldStatus s = status(0, 1);
    s.avgWaitMs = 90000;
    s.timeInQueueMs = 30000;
    b.statusReceived(s, Clock::now());
    CHECK(b.queues()[0].avgWaitTimeSec == 90);
    CHECK(b.queues()[0].timeInQueueSec == 30);
}

TEST_CASE("An invitation's countdown starts once and defaults to eighty seconds", "[battleground]") {
    Battlegrounds b;
    const auto first = Clock::now();
    b.statusReceived(status(0, 2), first);
    CHECK(b.queues()[0].inviteTimeout == 80);
    CHECK(b.hasPendingInvite());
    BattlefieldStatus restated = status(0, 2);
    restated.inviteTimeoutMs = 20000;
    b.statusReceived(restated, first + std::chrono::seconds(30));
    CHECK(b.queues()[0].inviteTimeout == 80);
    CHECK(b.queues()[0].inviteReceivedTime == first);
}

TEST_CASE("A listing replaces the battleground's last one", "[battleground]") {
    Battlegrounds b;
    AvailableBgInfo wsg;
    wsg.bgTypeId = 2;
    wsg.instanceIds = {1};
    b.listReceived(wsg);
    wsg.instanceIds = {1, 2};
    b.listReceived(wsg);
    AvailableBgInfo ab;
    ab.bgTypeId = 3;
    b.listReceived(ab);
    REQUIRE(b.available().size() == 2);
    CHECK(b.available()[0].instanceIds.size() == 2);
}

TEST_CASE("There is no scoreboard until one with players arrives", "[battleground]") {
    Battlegrounds b;
    CHECK_FALSE(b.scoreboard());
    b.scoreboardReceived(BgScoreboardData{});
    CHECK_FALSE(b.scoreboard());
    BgScoreboardData board;
    board.players.resize(3);
    CHECK(find<ScoreUpdated>(b.scoreboardReceived(board)));
    REQUIRE(b.scoreboard());
    CHECK(b.scoreboard()->players.size() == 3);
}

TEST_CASE("A join the server never answers is said after five seconds", "[battleground]") {
    Battlegrounds b;
    b.joinRequested(32);
    CHECK(b.timePassed(4.0f).empty());
    auto changes = b.timePassed(1.5f);
    REQUIRE(find<JoinUnanswered>(changes));
    CHECK(find<JoinUnanswered>(changes)->bgTypeId == 32);
    CHECK(b.timePassed(10.0f).empty());
}

TEST_CASE("A status answers a pending join", "[battleground]") {
    Battlegrounds b;
    b.joinRequested(32);
    b.statusReceived(status(0, 1, 32), Clock::now());
    CHECK(b.timePassed(10.0f).empty());
}

TEST_CASE("Positions are asked for at most once a second", "[battleground]") {
    Battlegrounds b;
    const auto t = Clock::now();
    CHECK(b.positionsRequestDue(t));
    CHECK_FALSE(b.positionsRequestDue(t + std::chrono::milliseconds(500)));
    CHECK(b.positionsRequestDue(t + std::chrono::milliseconds(1100)));
    // The second counts from the last request, not the first.
    CHECK_FALSE(b.positionsRequestDue(t + std::chrono::milliseconds(1600)));
}

TEST_CASE("Accepting goes into the invited queue", "[battleground]") {
    Fixture f;
    f.service.onStatus(status(0, 1, 3));
    f.service.onStatus(status(1, 2, 2));
    f.events.clear();
    f.service.acceptBattlefield();
    CHECK(f.gateway.sent == std::vector<std::string>{"enter 2"});
    CHECK(f.service.getBgQueues()[1].statusId == 3);
    CHECK_FALSE(f.service.hasPendingBgInvite());
    f.service.acceptBattlefield(0);
    CHECK(f.events == std::vector<std::string>{"none"});
}

TEST_CASE("Declining can leave a queue still waiting", "[battleground]") {
    Fixture f;
    f.service.onStatus(status(0, 1, 3));
    f.events.clear();
    f.service.declineBattlefield();
    CHECK(f.events == std::vector<std::string>{"none"});
    f.service.declineBattlefield(0);
    CHECK(f.gateway.sent == std::vector<std::string>{"refuse 3"});
    CHECK(f.events == std::vector<std::string>{"none", "left Arathi Basin", "slot 1"});
    CHECK(f.service.getBgQueues()[0].statusId == 0);
}

TEST_CASE("Declining an invitation says so", "[battleground]") {
    Fixture f;
    f.service.onStatus(status(2, 2, 2));
    f.events.clear();
    f.service.declineBattlefield();
    CHECK(f.events == std::vector<std::string>{"declined invite", "slot 3"});
}

TEST_CASE("A join waits for an answer", "[battleground]") {
    Fixture f;
    f.service.joinBattlefield(7, 32, 0, false);
    CHECK(f.gateway.sent == std::vector<std::string>{"join 32"});
    f.service.timePassed(6.0f);
    CHECK(f.events == std::vector<std::string>{"unanswered 32"});
}

TEST_CASE("A new character has none of the last one's queues, listings or scoreboard", "[battleground]") {
    Battlegrounds b;
    const auto t = Battlegrounds::Clock::now();
    BattlefieldStatus invited;
    invited.queueSlot = 0;
    invited.bgTypeId = 2;
    invited.statusId = 2;
    b.statusReceived(invited, t);
    AvailableBgInfo info;
    info.bgTypeId = 2;
    b.listReceived(info);
    BgScoreboardData board;
    board.players.push_back(BgPlayerScore{});
    b.scoreboardReceived(board);
    b.positionsReceived({BgPlayerPosition{}});
    b.joinRequested(3);
    REQUIRE(b.positionsRequestDue(t));

    b.beginCharacter();

    CHECK_FALSE(b.hasPendingInvite());
    CHECK(b.queues()[0].statusId == 0);
    CHECK(b.available().empty());
    CHECK(b.scoreboard() == nullptr);
    CHECK(b.positions().empty());
    CHECK(b.timePassed(10.0f).empty());
    // When positions were last asked for is the connection's, not the
    // character's: a second ask within the second still waits.
    CHECK_FALSE(b.positionsRequestDue(t));
}

TEST_CASE("A group join result answers the join", "[battleground]") {
    Fixture f;
    f.service.joinBattlefield(7, 32, 0, true);
    f.service.onJoinAcknowledged();
    f.service.timePassed(6.0f);
    CHECK(f.events.empty());
}

TEST_CASE("A refused queue says who could not come", "[battleground]") {
    Fixture f;
    f.gateway.names[9] = "Jaina";
    f.service.onJoinRefused(JoinRefusal::MemberUnavailable, -11, 9);
    f.service.onJoinRefused(JoinRefusal::MemberUnavailable, -11, 10);
    f.service.onJoinRefused(JoinRefusal::Deserter, -2, std::nullopt);
    f.service.onGroupQueued(2);
    CHECK(f.events == std::vector<std::string>{"refused -11 Jaina", "refused -11 A party member",
                                               "refused -2 A party member", "queued 2"});
}

TEST_CASE("Only named players are said to enter", "[battleground]") {
    Fixture f;
    f.gateway.names[9] = "Jaina";
    f.service.onPlayerEntered(9);
    f.service.onPlayerEntered(10);
    CHECK(f.events == std::vector<std::string>{"entered Jaina"});
}

TEST_CASE("Positions are asked for only on a battleground", "[battleground]") {
    Fixture f;
    f.gateway.battleground = false;
    f.service.requestBattlefieldPositions();
    CHECK(f.gateway.sent.empty());
    f.gateway.battleground = true;
    f.service.requestBattlefieldPositions();
    f.service.requestBattlefieldPositions();
    CHECK(f.gateway.sent == std::vector<std::string>{"positions"});
}

TEST_CASE("Nothing is sent while offline, and an AFK report needs someone", "[battleground]") {
    Fixture f;
    f.service.reportPvpAfk(0);
    CHECK(f.gateway.sent.empty());
    f.gateway.world = false;
    f.service.joinBattlefield(7, 2, 0, false);
    f.service.leaveBattlefield();
    f.service.requestPvpLog();
    f.service.requestBattlefieldList(2);
    f.service.reportPvpAfk(5);
    CHECK(f.gateway.sent.empty());
    f.service.timePassed(10.0f);
    CHECK(f.events.empty());
}
