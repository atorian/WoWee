#include <catch_amalgamated.hpp>
#include "pipeline/preparation_cache.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using wowee::pipeline::PreparationCache;
using namespace std::chrono_literals;
namespace {
const auto cost = [](const std::vector<int>& v) { return v.capacity() * sizeof(int); };
}

TEST_CASE("parallel preparation shares one result and releases the lock during work", "[assets][preparation]") {
    PreparationCache<std::vector<int>> cache(4096);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<size_t> produced = 0;
    auto loader = [&](uint64_t) {
        if (++produced == 1) entered.set_value();
        gate.wait();
        return std::vector<int>{7, 8, 9};
    };
    auto first = std::async(std::launch::async, [&] { return cache.get("stone", loader, cost); });
    entered.get_future().wait();
    auto second = std::async(std::launch::async, [&] { return cache.get("stone", loader, cost); });
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!cache.stats().joined && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    const bool joined = cache.stats().joined != 0;
    // A different texture completes even while the first texture's I/O is blocked.
    auto other = cache.get("wood", [](uint64_t) { return std::vector<int>{3}; }, cost);
    release.set_value();
    const auto a = first.get(), b = second.get();
    CHECK(joined);
    CHECK(produced == 1);
    CHECK(a == b);
    CHECK(*a == std::vector<int>{7, 8, 9});
    CHECK((*other)[0] == 3);
}

TEST_CASE("cache invalidation cannot publish or join an older in-flight texture", "[assets][preparation]") {
    PreparationCache<std::vector<int>> cache(4096);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto old = std::async(std::launch::async, [&] {
        return cache.get("stone", [&](uint64_t) {
            entered.set_value(); gate.wait(); return std::vector<int>{1};
        }, cost);
    });
    entered.get_future().wait();
    cache.clear();
    auto fresh = cache.get("stone", [](uint64_t) { return std::vector<int>{2}; }, cost);
    release.set_value();
    auto pinnedOld = old.get();
    auto again = cache.get("stone", [](uint64_t) { return std::vector<int>{999}; }, cost);
    CHECK((*pinnedOld)[0] == 1);
    CHECK((*fresh)[0] == 2);
    CHECK(again == fresh);
}

TEST_CASE("preparation LRU bounds retained bytes while queued readers pin evicted data", "[assets][preparation]") {
    PreparationCache<std::vector<int>> cache(4096, 2);
    size_t loads = 0;
    auto loader = [&](uint64_t) { return std::vector<int>{int(++loads)}; };
    auto a = cache.get("a", loader, cost);
    auto b = cache.get("b", loader, cost);
    CHECK(cache.get("a", loader, cost) == a);
    cache.get("c", loader, cost);
    CHECK(cache.get("a", loader, cost) == a);
    CHECK((*b)[0] == 2); // Still in the queued upload's ownership after eviction.
    CHECK(cache.get("b", loader, cost) != b);
    cache.setBudget(0);
    CHECK(cache.stats().bytes == 0);
    CHECK((*a)[0] == 1);
    auto uncached = cache.get("a", loader, cost);
    CHECK(cache.get("a", loader, cost) != uncached);
    CHECK(cache.stats().entries == 0);
}

TEST_CASE("throwing preparation releases waiters and allows a later retry", "[assets][preparation]") {
    PreparationCache<std::vector<int>> cache(4096);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<size_t> produced = 0;
    auto loader = [&](uint64_t) -> std::vector<int> {
        if (++produced == 1) entered.set_value();
        gate.wait();
        throw std::runtime_error("failed source");
    };
    auto producer = std::async(std::launch::async, [&] { return cache.get("bad", loader, cost); });
    entered.get_future().wait();
    auto waiter = std::async(std::launch::async, [&] { return cache.get("bad", loader, cost); });
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!cache.stats().joined && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    const bool joined = cache.stats().joined != 0;
    release.set_value();
    CHECK_THROWS_WITH(producer.get(), "failed source");
    CHECK_THROWS_WITH(waiter.get(), "failed source");
    CHECK(joined);
    CHECK(produced == 1);
    CHECK(cache.stats().active == 0);
    auto recovered = cache.get("bad", [](uint64_t) { return std::vector<int>{4}; }, cost);
    CHECK((*recovered)[0] == 4);
}
