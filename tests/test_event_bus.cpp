#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"

#include <string>
#include <thread>
#include <vector>

using wowee::core::EventBus;

namespace {
struct Ping { int value = 0; };
struct Pong { std::string text; };
}

// The reason for the bus: two parts of the client care about the same event.
// With one callback slot per event the second to register replaced the first.
TEST_CASE("every subscriber hears an event", "[events]") {
    EventBus bus;
    std::vector<int> heard;
    auto a = bus.subscribe<Ping>([&](const Ping& p) { heard.push_back(p.value); });
    auto b = bus.subscribe<Ping>([&](const Ping& p) { heard.push_back(p.value * 10); });

    bus.publish(Ping{3});
    CHECK(heard == std::vector<int>{3, 30});
}

TEST_CASE("events of another type are not delivered", "[events]") {
    EventBus bus;
    int pings = 0;
    auto sub = bus.subscribe<Ping>([&](const Ping&) { ++pings; });
    bus.publish(Pong{"x"});
    CHECK(pings == 0);
}

TEST_CASE("a subscription ends with its handle", "[events]") {
    EventBus bus;
    int pings = 0;
    {
        auto sub = bus.subscribe<Ping>([&](const Ping&) { ++pings; });
        bus.publish(Ping{});
    }
    bus.publish(Ping{});
    CHECK(pings == 1);
}

TEST_CASE("a moved handle keeps the subscription", "[events]") {
    EventBus bus;
    int pings = 0;
    EventBus::Subscription kept;
    {
        auto sub = bus.subscribe<Ping>([&](const Ping&) { ++pings; });
        kept = std::move(sub);
    }
    bus.publish(Ping{});
    CHECK(pings == 1);
    kept.reset();
    bus.publish(Ping{});
    CHECK(pings == 1);
}

// A handler that ends its own subscription is still running when it does; the
// handler must not be destroyed under it.
TEST_CASE("a handler may end its own subscription", "[events]") {
    EventBus bus;
    int pings = 0;
    EventBus::Subscription sub;
    std::string captured = "kept alive while running";
    sub = bus.subscribe<Ping>([&, captured](const Ping&) {
        sub.reset();
        ++pings;
        CHECK(captured == "kept alive while running");
    });
    bus.publish(Ping{});
    bus.publish(Ping{});
    CHECK(pings == 1);
}

TEST_CASE("a subscriber added during delivery waits for the next event", "[events]") {
    EventBus bus;
    int late = 0;
    std::vector<EventBus::Subscription> subs;
    subs.push_back(bus.subscribe<Ping>([&](const Ping&) {
        if (subs.size() < 50) {
            subs.push_back(bus.subscribe<Ping>([&](const Ping&) { ++late; }));
        }
    }));
    bus.publish(Ping{});
    CHECK(late == 0);
    bus.publish(Ping{});
    CHECK(late == 1);
}

TEST_CASE("posted events wait for dispatch, in order", "[events]") {
    EventBus bus;
    std::vector<int> heard;
    auto sub = bus.subscribe<Ping>([&](const Ping& p) { heard.push_back(p.value); });

    bus.post(Ping{1});
    bus.post(Ping{2});
    CHECK(heard.empty());

    bus.dispatchPosted();
    CHECK(heard == std::vector<int>{1, 2});
}

TEST_CASE("an event posted while dispatching waits for the next dispatch", "[events]") {
    EventBus bus;
    int pongs = 0;
    auto ping = bus.subscribe<Ping>([&](const Ping&) { bus.post(Pong{}); });
    auto pong = bus.subscribe<Pong>([&](const Pong&) { ++pongs; });

    bus.post(Ping{});
    bus.dispatchPosted();
    CHECK(pongs == 0);
    bus.dispatchPosted();
    CHECK(pongs == 1);
}

// The network thread's way in: it posts, the main thread delivers.
TEST_CASE("events posted from another thread arrive on dispatch", "[events]") {
    EventBus bus;
    int total = 0;
    auto sub = bus.subscribe<Ping>([&](const Ping& p) { total += p.value; });

    std::thread worker([&] {
        for (int i = 1; i <= 100; ++i) bus.post(Ping{i});
    });
    worker.join();

    bus.dispatchPosted();
    CHECK(total == 5050);
}
