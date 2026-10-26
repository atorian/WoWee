// Tool calls run on the frame loop's thread, or not at all.
//
// The listener's thread queues a call and waits; the main loop runs what is
// queued once a frame. A call nobody drained in time is refused and must
// never run afterwards - the agent was already told it did not happen.
#include <catch_amalgamated.hpp>

#include "mcp/main_thread_queue.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using wowee::mcp::MainThreadQueue;
using wowee::mcp::ToolResult;
using namespace std::chrono_literals;

TEST_CASE("a call runs on the thread that drains, and its result comes back", "[mcp]") {
    MainThreadQueue queue;
    std::thread::id ranOn;
    ToolResult result;
    std::thread caller([&] {
        result = queue.run([&] { ranOn = std::this_thread::get_id(); return ToolResult::text("done"); }, 2s);
    });
    // The "main loop": drain until the call has been taken.
    for (int i = 0; i < 200 && ranOn == std::thread::id(); ++i) {
        queue.drain();
        std::this_thread::sleep_for(5ms);
    }
    caller.join();
    CHECK(ranOn == std::this_thread::get_id());
    CHECK_FALSE(result.isError);
    CHECK(result.content[0].at("text") == "done");
}

TEST_CASE("a call nobody drained in time is refused and never runs later", "[mcp]") {
    MainThreadQueue queue;
    std::atomic<bool> ran{false};
    const auto result = queue.run([&] { ran = true; return ToolResult::text("late"); }, 20ms);
    CHECK(result.isError);
    queue.drain();
    CHECK_FALSE(ran);
}

TEST_CASE("a task that throws comes back as a tool error", "[mcp]") {
    MainThreadQueue queue;
    ToolResult result;
    std::atomic<bool> done{false};
    std::thread caller([&] {
        result = queue.run([]() -> ToolResult { throw std::runtime_error("no world"); }, 2s);
        done = true;
    });
    for (int i = 0; i < 200 && !done; ++i) {
        queue.drain();
        std::this_thread::sleep_for(5ms);
    }
    caller.join();
    CHECK(result.isError);
    CHECK(result.content[0].at("text") == "no world");
}
