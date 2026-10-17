// Normal maps are built on a few workers of the builder's own, nearest first,
// from a queue that never grows past its capacity: a nearer request takes the
// place of the farthest, a farther one is refused and asked for again later.
// The derivation is a stand-in that records the order it ran in.
#include <catch_amalgamated.hpp>

#include "rendering/normal_map_builder.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using wowee::rendering::NormalMapBuilder;

namespace {

/// Holds the worker inside its first job until the test lets it go.
class Gate {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

NormalMapBuilder::Source pixels(Gate* gate = nullptr) {
    return [gate]() -> std::optional<NormalMapBuilder::Pixels> {
        if (gate) gate->wait();
        return NormalMapBuilder::Pixels{std::vector<uint8_t>(4 * 4 * 4, 128), 4, 4};
    };
}

NormalMapBuilder::Derive flat() {
    return [](const uint8_t*, uint32_t w, uint32_t h, float& variance) {
        variance = 0.5f;
        return std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 255);
    };
}

template <typename Done>
void waitFor(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(done());
}

std::vector<std::string> takeAll(NormalMapBuilder& builder, size_t count) {
    std::vector<std::string> keys;
    waitFor([&] {
        for (auto& r : builder.takeFinished(64)) keys.push_back(r.key);
        return keys.size() >= count;
    });
    return keys;
}

} // namespace

TEST_CASE("the queue never holds more than its capacity, and the nearest keep their places",
          "[normal_maps]") {
    Gate gate;
    NormalMapBuilder builder(1, 3, flat());
    using Submitted = NormalMapBuilder::Submitted;

    // The one worker takes the first request and is held inside it.
    REQUIRE(builder.submit({"busy", 100.0f, pixels(&gate)}) == Submitted::Queued);
    waitFor([&] { return builder.stats().running == 1; });

    CHECK(builder.submit({"b", 50.0f, pixels()}) == Submitted::Queued);
    CHECK(builder.submit({"c", 60.0f, pixels()}) == Submitted::Queued);
    CHECK(builder.submit({"d", 70.0f, pixels()}) == Submitted::Queued);
    // Full: a farther one is refused, a nearer one takes the farthest's place.
    CHECK(builder.submit({"e", 80.0f, pixels()}) == Submitted::Refused);
    CHECK(builder.submit({"near", 10.0f, pixels()}) == Submitted::Queued);
    // Asked again: already there.
    CHECK(builder.submit({"b", 40.0f, pixels()}) == Submitted::AlreadyThere);

    const auto stats = builder.stats();
    CHECK(stats.queued == 3);
    CHECK(stats.displaced == 1);
    CHECK(stats.refused == 1);
    CHECK_FALSE(builder.contains("d"));
    CHECK_FALSE(builder.contains("e"));
    CHECK(builder.contains("near"));

    // Let it go: nearest first.
    gate.open();
    const auto order = takeAll(builder, 4);
    CHECK(order == std::vector<std::string>{"busy", "near", "b", "c"});
    // Taken, they are no longer there; a displaced one can be asked for again.
    CHECK_FALSE(builder.contains("near"));
    CHECK(builder.submit({"d", 70.0f, pixels()}) == Submitted::Queued);
}

TEST_CASE("a map whose texture cannot be read comes back failed rather than never",
          "[normal_maps]") {
    NormalMapBuilder builder(2, 8, flat());
    builder.submit({"missing", 1.0f, [] { return std::optional<NormalMapBuilder::Pixels>{}; }});
    builder.submit({"present", 2.0f, pixels()});

    std::vector<NormalMapBuilder::Result> results;
    waitFor([&] {
        for (auto& r : builder.takeFinished(8)) results.push_back(std::move(r));
        return results.size() == 2;
    });
    for (const auto& r : results) {
        if (r.key == "missing") {
            CHECK(r.failed);
        } else {
            CHECK_FALSE(r.failed);
            CHECK(r.width == 4);
            CHECK(r.pixels.size() == 4 * 4 * 4);
            CHECK(r.variance == 0.5f);
        }
    }
}
