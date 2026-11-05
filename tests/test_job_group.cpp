#include <catch_amalgamated.hpp>
#include "foundation/job_group.hpp"
#include "client/asset_requests.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using wowee::core::JobGroup;
using wowee::core::AssetRequests;

TEST_CASE("map destruction waits until a worker has stopped using its owner's data", "[map][jobs]") {
    std::promise<void> started;
    std::atomic<bool> finished{false};
    auto map = std::make_unique<JobGroup>();
    auto work = map->submit([&](const JobGroup::Token& token) {
        started.set_value();
        while (token.current()) std::this_thread::yield();
        finished.store(true, std::memory_order_release);
        return 73;
    });
    started.get_future().wait();
    map.reset();
    CHECK(finished.load(std::memory_order_acquire));
    CHECK_FALSE(work.current());
    CHECK(work.get() == 73);
}

TEST_CASE("a despawn followed by reuse of the same GUID cannot accept the old appearance", "[map][jobs]") {
    JobGroup map;
    AssetRequests requests(map);
    const auto old = requests.replace(AssetRequests::Channel::Player, 42);
    auto work = map.submit(old, [](const auto&) { return std::string("old appearance"); });
    work.wait(); // It can be ready before despawn, but not yet consumed.
    requests.cancel(AssetRequests::Channel::Player, 42);
    const auto replacement = requests.replace(AssetRequests::Channel::Player, 42);
    CHECK_FALSE(map.accepts(work.token));
    CHECK(map.accepts(replacement));
}

TEST_CASE("replacing equipment does not cancel the same GUID's model preparation", "[map][jobs]") {
    JobGroup map;
    AssetRequests requests(map);
    const auto model = requests.replace(AssetRequests::Channel::Player, 42);
    const auto oldGear = requests.replace(AssetRequests::Channel::Equipment, 42);
    auto gear = map.submit(oldGear, [](const auto&) { return std::string("old gear"); });
    gear.wait();
    requests.replace(AssetRequests::Channel::Equipment, 42);
    CHECK_FALSE(map.accepts(gear.token));
    CHECK(map.accepts(model));
}

TEST_CASE("a new map rejects completed work from an older map with the same GUID", "[map][jobs]") {
    JobGroup previous, next;
    AssetRequests previousRequests(previous), nextRequests(next);
    auto work = previous.submit(previousRequests.replace(AssetRequests::Channel::Creature, 42),
                               [](const auto&) { return std::string("old creature"); });
    work.wait();
    nextRequests.replace(AssetRequests::Channel::Creature, 42);
    CHECK_FALSE(next.accepts(work.token));
    CHECK(previous.accepts(work.token));
}

TEST_CASE("worker failures remain visible to their consumer and release the lifetime fence", "[map][jobs]") {
    JobGroup map;
    auto work = map.submit([](const auto&) -> int { throw std::runtime_error("missing model"); });
    CHECK_THROWS_WITH(work.get(), "missing model");
    map.cancel();
    map.wait();
}

TEST_CASE("a persistent worker's step holds the map open, and none starts once it is cancelled",
          "[map][jobs]") {
    // The terrain workers are threads of their own: each tile they prepare is
    // a step tracked against the map, not a pooled task.
    JobGroup map;
    const auto token = map.token();
    auto step = JobGroup::track(token);
    REQUIRE(step);
    std::atomic<bool> joined{false};
    std::thread teardown([&] {
        map.cancel();
        map.wait();
        joined.store(true, std::memory_order_release);
    });
    while (token.current()) std::this_thread::yield();
    CHECK_FALSE(JobGroup::track(token));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(joined.load(std::memory_order_acquire));
    { auto finished = std::move(step); }
    teardown.join();
    CHECK(joined.load(std::memory_order_acquire));
}

TEST_CASE("ending a map invalidates every request made on it", "[map][jobs]") {
    JobGroup map;
    AssetRequests requests(map);
    const auto creature = requests.replace(AssetRequests::Channel::Creature, 7);
    const auto gear = requests.request(AssetRequests::Channel::Equipment, 8);
    CHECK(requests.request(AssetRequests::Channel::Equipment, 8).current());
    map.cancel();
    CHECK_FALSE(creature.current());
    CHECK_FALSE(gear.current());
    CHECK_FALSE(map.accepts(creature));
    CHECK_FALSE(map.accepts(map.token()));
}
