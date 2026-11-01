// Duels: the challenge, the countdown and the answers, and the service against
// a recording gateway and the real event bus. Only wowee_duel is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "duel/services/duel_service.hpp"

#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::duel;

namespace {

using Clock = Duel::Clock;
constexpr uint64_t kSelf = 1;
constexpr uint64_t kFlag = 77;

struct RecordingGateway final : IDuelGateway, IDuelWorld {
    bool world = true;
    std::map<uint64_t, std::string> names;
    std::vector<std::string> sent;

    bool online() const override { return world; }
    void sendAccept(uint64_t a) override { sent.push_back("accept " + std::to_string(a)); }
    void sendCancel(uint64_t a) override { sent.push_back("cancel " + std::to_string(a)); }
    void sendPropose(uint64_t t) override { sent.push_back("propose " + std::to_string(t)); }
    uint64_t playerGuid() const override { return kSelf; }
    std::string name(uint64_t guid) const override {
        auto it = names.find(guid);
        return it == names.end() ? std::string() : it->second;
    }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    DuelService service{gateway, gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<DuelRequested>(
            [this](const DuelRequested& e) { events.push_back("challenged by " + e.challengerName); }));
        subscriptions.push_back(bus.subscribe<NoDuelTarget>([this](const auto&) { events.push_back("no target"); }));
        subscriptions.push_back(bus.subscribe<DuelAccepted>([this](const auto&) { events.push_back("accepted"); }));
    }
};

} // namespace

TEST_CASE("A challenge this player issued asks nothing of them", "[duel]") {
    Duel d;
    CHECK(d.challenged(kFlag, kSelf, kSelf, "Me").empty());
    CHECK_FALSE(d.pending());
    CHECK(d.arbiter() == kFlag);
    auto changes = d.challenged(kFlag, 5, kSelf, "Garrosh");
    REQUIRE(changes.size() == 1);
    CHECK(std::get<DuelRequested>(changes[0]).challengerName == "Garrosh");
    CHECK(d.pending());
}

TEST_CASE("A new character is asked nothing of the last one's duel", "[duel]") {
    Duel d;
    const auto t = Clock::now();
    d.challenged(kFlag, 5, kSelf, "Garrosh");
    d.countdownStarted(3000, t);
    d.beginCharacter();
    CHECK_FALSE(d.pending());
    CHECK(d.arbiter() == 0);
    CHECK(d.countdownRemaining(t) == 0.0f);
}

TEST_CASE("The countdown runs down from what the server gave, or three seconds", "[duel]") {
    Duel d;
    const auto t = Clock::now();
    CHECK(d.countdownRemaining(t) == 0.0f);
    d.countdownStarted(5000, t);
    CHECK(d.countdownRemaining(t + std::chrono::seconds(2)) == Catch::Approx(3.0f).margin(0.01));
    CHECK(d.countdownRemaining(t + std::chrono::seconds(6)) == 0.0f);
    d.countdownStarted(0, t);
    CHECK(d.countdownRemaining(t) == Catch::Approx(3.0f).margin(0.01));
    d.countdownStarted(60000, t);
    CHECK(d.countdownRemaining(t) == Catch::Approx(3.0f).margin(0.01));
}

TEST_CASE("An ended duel stops its countdown and its question", "[duel]") {
    Duel d;
    const auto t = Clock::now();
    d.challenged(kFlag, 5, kSelf, "Garrosh");
    d.countdownStarted(3000, t);
    auto changes = d.completed(false);
    CHECK_FALSE(d.pending());
    CHECK(d.countdownRemaining(t) == 0.0f);
    CHECK_FALSE(std::get<DuelFinished>(changes[0]).started);
}

TEST_CASE("A challenger is named, by the hex of the guid when unknown", "[duel]") {
    Fixture f;
    f.gateway.names[5] = "Garrosh";
    f.service.onRequested(kFlag, 5);
    f.service.onRequested(kFlag, 0x2A);
    CHECK(f.events == std::vector<std::string>{"challenged by Garrosh", "challenged by 0x2A"});
}

TEST_CASE("Accepting answers the challenge once, naming the flag", "[duel]") {
    Fixture f;
    f.service.acceptDuel();
    CHECK(f.gateway.sent.empty());
    f.service.onRequested(kFlag, 5);
    f.service.acceptDuel();
    f.service.acceptDuel();
    CHECK(f.gateway.sent == std::vector<std::string>{"accept 77"});
}

TEST_CASE("Forfeiting names the flag and ends the question", "[duel]") {
    Fixture f;
    f.service.onRequested(kFlag, 5);
    f.service.forfeitDuel();
    f.service.acceptDuel();
    CHECK(f.gateway.sent == std::vector<std::string>{"cancel 77"});
}

TEST_CASE("A challenge needs someone to challenge", "[duel]") {
    Fixture f;
    f.service.proposeDuel(0);
    f.service.proposeDuel(9);
    CHECK(f.gateway.sent == std::vector<std::string>{"propose 9"});
    CHECK(f.events == std::vector<std::string>{"no target"});
}

TEST_CASE("Nothing is sent while offline", "[duel]") {
    Fixture f;
    f.service.onRequested(kFlag, 5);
    f.gateway.world = false;
    f.service.acceptDuel();
    f.service.forfeitDuel();
    f.service.proposeDuel(9);
    CHECK(f.gateway.sent.empty());
}
