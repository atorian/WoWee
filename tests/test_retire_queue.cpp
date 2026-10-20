// Releases held back until the GPU has finished every frame that could use
// what they release (rendering::RetireQueue). The render thread records a
// frame from a snapshot the game took earlier, so a release has to wait for
// that frame by its number - not for whichever frame slot happened to be
// current when the game let the object go.
#include <catch_amalgamated.hpp>

#include "rendering/retire_queue.hpp"

#include <atomic>
#include <thread>
#include <vector>

using wowee::rendering::RetireQueue;

TEST_CASE("a release waits for the last frame that may use it", "[retire]") {
    RetireQueue queue;
    bool released = false;
    queue.retireAfter(5, [&] { released = true; });

    queue.runCompleted(4);
    CHECK_FALSE(released);
    queue.runCompleted(5);
    CHECK(released);
}

TEST_CASE("releases run once, in the order they were filed", "[retire]") {
    RetireQueue queue;
    std::vector<int> order;
    queue.retireAfter(3, [&] { order.push_back(1); });
    queue.retireAfter(2, [&] { order.push_back(2); });
    queue.retireAfter(7, [&] { order.push_back(3); });

    queue.runCompleted(3);
    queue.runCompleted(3);
    CHECK(order == std::vector<int>{1, 2});
    queue.runCompleted(10);
    CHECK(order == std::vector<int>{1, 2, 3});
    CHECK(queue.pending() == 0);
}

TEST_CASE("a release may file another, which waits its own turn", "[retire]") {
    RetireQueue queue;
    bool inner = false;
    queue.retireAfter(1, [&] { queue.retireAfter(4, [&] { inner = true; }); });

    queue.runCompleted(1);
    CHECK_FALSE(inner);
    queue.runCompleted(4);
    CHECK(inner);
}

TEST_CASE("with no frame left in flight everything is released", "[retire]") {
    RetireQueue queue;
    int released = 0;
    queue.retireAfter(100, [&] {
        ++released;
        queue.retireAfter(200, [&] { ++released; });
    });
    CHECK(queue.runAll() == 2);
    CHECK(released == 2);
}

TEST_CASE("releases filed from several threads all run exactly once", "[retire]") {
    RetireQueue queue;
    std::atomic<int> released{0};
    constexpr int kThreads = 4;
    constexpr int kEach = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i) {
                queue.retireAfter(static_cast<uint64_t>(i % 10), [&] { ++released; });
            }
            (void)t;
        });
    }
    // Completions reported while the threads are still filing.
    for (uint64_t frame = 0; frame < 10; ++frame) queue.runCompleted(frame);
    for (auto& thread : threads) thread.join();
    queue.runCompleted(10);
    CHECK(released.load() == kThreads * kEach);
}
