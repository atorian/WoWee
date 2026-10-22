// The ranges terrain chunks' geometry is copied into, in the buffers many
// tiles share. Tiles stream in and out all session long, so what a tile gives
// back must be there for the next tile - it used to be handed out once and
// never again, and after a few dozen tiles every new chunk went without.
#include <catch_amalgamated.hpp>

#include "rendering/range_allocator.hpp"

#include <vector>
#include <array>
#include <atomic>
#include <barrier>
#include <thread>

using wowee::rendering::RangeAllocator;

TEST_CASE("a range given back is handed out again", "[terrain][allocator]") {
    RangeAllocator ranges(1000);
    const auto a = ranges.allocate(600);
    REQUIRE(a);
    CHECK_FALSE(ranges.allocate(600));  // full: 400 left

    ranges.free(*a, 600);
    const auto again = ranges.allocate(600);
    REQUIRE(again);
    CHECK(ranges.used() == 600);
}

TEST_CASE("streaming tiles through never runs out of room", "[terrain][allocator]") {
    // Room for four tiles at a time; a hundred pass through.
    constexpr uint32_t kTile = 256 * 145;
    RangeAllocator ranges(4 * kTile);
    std::vector<uint32_t> resident;
    for (int tile = 0; tile < 100; ++tile) {
        if (resident.size() == 4) {
            ranges.free(resident.front(), kTile);
            resident.erase(resident.begin());
        }
        const auto at = ranges.allocate(kTile);
        REQUIRE(at);
        resident.push_back(*at);
    }
    CHECK(ranges.used() == 4 * kTile);
}

TEST_CASE("neighbouring ranges given back join into one", "[terrain][allocator]") {
    RangeAllocator ranges(300);
    const auto a = ranges.allocate(100);
    const auto b = ranges.allocate(100);
    const auto c = ranges.allocate(100);
    REQUIRE((a && b && c));

    // Given back out of order, the three are one range again.
    ranges.free(*a, 100);
    ranges.free(*c, 100);
    CHECK_FALSE(ranges.allocate(300));
    ranges.free(*b, 100);
    CHECK(ranges.freeRangeCount() == 1);
    CHECK(ranges.largestFree() == 300);
    CHECK(ranges.allocate(300));
}

TEST_CASE("handed out ranges never overlap", "[terrain][allocator]") {
    RangeAllocator ranges(10000);
    struct Range { uint32_t at, count; };
    std::vector<Range> live;
    // Allocate and give back in an uneven pattern, checking every live pair.
    for (uint32_t step = 0; step < 400; ++step) {
        const uint32_t count = 7 + (step * 37) % 211;
        if (const auto at = ranges.allocate(count)) live.push_back({*at, count});
        if (step % 3 == 0 && !live.empty()) {
            const size_t victim = (step * 13) % live.size();
            ranges.free(live[victim].at, live[victim].count);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
        }
    }
    for (size_t i = 0; i < live.size(); ++i) {
        CHECK(live[i].at + live[i].count <= ranges.capacity());
        for (size_t j = i + 1; j < live.size(); ++j) {
            const bool apart = live[i].at + live[i].count <= live[j].at ||
                               live[j].at + live[j].count <= live[i].at;
            CHECK(apart);
        }
    }
}

TEST_CASE("streaming allocations and fence returns preserve exclusive ranges", "[terrain][allocator][threaded]") {
    constexpr uint32_t capacity = 1024;
    RangeAllocator ranges(capacity);
    std::array<std::atomic<unsigned>, capacity> owners{};
    std::atomic<bool> valid{true};
    std::barrier start(3);
    auto churn = [&](unsigned owner, uint32_t count) {
        start.arrive_and_wait();
        for (int i = 0; i < 20000 && valid.load(); ++i) {
            const auto at = ranges.allocate(count);
            if (!at || *at > capacity - count) { valid = false; break; }
            for (uint32_t slot = *at; slot < *at + count; ++slot)
                if (owners[slot].exchange(owner) != 0) valid = false;
            for (uint32_t slot = *at; slot < *at + count; ++slot)
                if (owners[slot].exchange(0) != owner) valid = false;
            ranges.free(*at, count);
        }
    };
    std::thread producer(churn, 1, 7);
    std::thread retiring(churn, 2, 13);
    start.arrive_and_wait();
    for (int i = 0; i < 20000 && valid.load(); ++i) {
        if (ranges.used() > capacity || ranges.largestFree() > capacity ||
            ranges.freeRangeCount() > capacity || ranges.capacity() != capacity) valid = false;
    }
    producer.join();
    retiring.join();
    REQUIRE(valid.load());
    CHECK(ranges.used() == 0);
    CHECK(ranges.allocate(capacity) == 0);
}
