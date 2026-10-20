#include <catch_amalgamated.hpp>
#include "core/frame_worker.hpp"
#include <future>
#include <memory>
#include <vector>
using wowee::core::FrameWorker;
using namespace std::chrono_literals;

TEST_CASE("recording overlaps the producer but never admits a third snapshot", "[frame-worker]") {
    FrameWorker worker(true);
    std::promise<void> entered, release, reserving;
    auto gate = release.get_future();
    std::vector<int> order;
    auto scene = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = scene;
    worker.submit(worker.reserve(), [&, scene = std::move(scene)] {
        entered.set_value(); gate.wait(); order.push_back(*scene);
    });
    entered.get_future().wait();
    worker.submit(worker.reserve(), [&] { order.push_back(8); });
    auto third = std::async(std::launch::async, [&] {
        reserving.set_value();
        return worker.reserve();
    });
    reserving.get_future().wait();
    CHECK(third.wait_for(20ms) == std::future_status::timeout);
    CHECK_FALSE(lifetime.expired());
    release.set_value();
    auto permit = third.get();
    worker.submit(std::move(permit), [&] { order.push_back(9); });
    worker.drain();
    CHECK(lifetime.expired());
    CHECK(order == std::vector<int>{7, 8, 9});
}

TEST_CASE("inline and threaded workers execute the same ordered owned requests", "[frame-worker]") {
    for (const bool threaded : {false, true}) {
        std::vector<int> values;
        const auto producer = std::this_thread::get_id();
        bool correctThread = true;
        FrameWorker worker(threaded);
        { auto cancelledExtraction = worker.reserve(); }
        for (int i = 0; i < 10; ++i) {
            worker.submit(worker.reserve(), [&, i] {
                correctThread &= (std::this_thread::get_id() != producer) == threaded;
                values.push_back(i);
            });
        }
        worker.drain();
        CHECK(correctThread);
        CHECK(values == std::vector<int>{0,1,2,3,4,5,6,7,8,9});
    }
}

TEST_CASE("recording failure cancels queued frames and reaches the producer", "[frame-worker]") {
    FrameWorker worker(true);
    std::promise<void> entered, release;
    auto gate = release.get_future();
    auto scene = std::make_shared<int>(2);
    std::weak_ptr<int> lifetime = scene;
    bool laterRan = false;
    worker.submit(worker.reserve(), [&] {
        entered.set_value(); gate.wait(); throw std::runtime_error("recording failed");
    });
    entered.get_future().wait();
    worker.submit(worker.reserve(), [&, scene = std::move(scene)] { laterRan = true; });
    release.set_value();
    CHECK_THROWS_WITH(worker.drain(), "recording failed");
    CHECK_FALSE(laterRan);
    CHECK(lifetime.expired());
    CHECK_THROWS_WITH(worker.reserve(), "recording failed");
    worker.stop();
}

TEST_CASE("stopping joins after the last accepted frame and releases its scene", "[frame-worker]") {
    FrameWorker worker(true);
    auto scene = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = scene;
    int answer = 0;
    worker.submit(worker.reserve(), [&, scene = std::move(scene)] { answer = *scene; });
    worker.stop();
    CHECK(answer == 42);
    CHECK(lifetime.expired());
    CHECK_THROWS(worker.reserve());
}
