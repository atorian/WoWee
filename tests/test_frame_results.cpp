#include <catch_amalgamated.hpp>
#include "rendering/frame_results.hpp"
#include <future>
#include <thread>

using namespace wowee::rendering;

TEST_CASE("GPU answers stay queued until the consumer boundary and arrive once", "[frame-results]") {
    FrameResultMailbox mailbox;
    std::promise<void> firstSent, resume;
    auto gate = resume.get_future();
    std::thread producer([&] {
        FrameResults first;
        std::string graph = "old graph";
        first.gameFrame = 12; first.recording = 10;
        first.cpu.push_back({"recording", graph, 3.5f});
        first.sun = {8, 2, 0.25f};
        mailbox.publish(std::move(first));
        graph[0] = 'X';
        first.cpu = {{"changed", "new graph", 17}};
        firstSent.set_value();
        gate.wait();
        FrameResults next;
        next.gameFrame = 13; next.recording = 11;
        next.sun = {9, 2, 0.75f};
        mailbox.publish(std::move(next));
    });
    firstSent.get_future().wait();
    auto ready = mailbox.take();
    CHECK(ready.size() == 1);
    if (!ready.empty()) {
        CHECK(ready.front().cpu.front().name == "old graph");
        CHECK(ready.front().gameFrame == 12);
        CHECK(ready.front().sun.gpuFrame == 8);
        CHECK(ready.front().sun.value == 0.25f);
    }
    CHECK(mailbox.take().empty());
    resume.set_value();
    producer.join();
    const auto next = mailbox.take();
    REQUIRE(next.size() == 1);
    CHECK(next.front().gameFrame == 13);
    CHECK(next.front().sun.value == 0.75f);
    CHECK(mailbox.take().empty());
}

TEST_CASE("queued readback buffers survive the GPU owner until the consumer releases them", "[frame-results]") {
    FrameResultMailbox mailbox;
    bool released = false;
    uint8_t byte = 93;
    {
        FrameResults result;
        CapturedImage image;
        image.pixels = std::shared_ptr<const uint8_t>(&byte, [&](const uint8_t*) { released = true; });
        result.images.push_back(std::move(image));
        mailbox.publish(std::move(result));
    }
    CHECK_FALSE(released);
    {
        const auto result = mailbox.take();
        REQUIRE(result.size() == 1);
        REQUIRE(result.front().images.size() == 1);
        CHECK(*result.front().images.front().pixels == 93);
        CHECK_FALSE(released);
    }
    CHECK(released);
}
