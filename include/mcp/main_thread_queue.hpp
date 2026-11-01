#pragma once

#include "mcp/mcp_server.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace wowee {
namespace mcp {

/// Hands tool calls from the server's thread to the frame loop.
///
/// The game, the renderer and the input are touched only by the main thread;
/// a tool call that reached them from the listener would race the frame. run()
/// queues the work and waits for drain(), which the main loop calls once a
/// frame. Work still waiting when the time runs out is dropped, never run
/// late: a move the agent has already been told failed must not happen.
class MainThreadQueue {
public:
    MainThreadQueue();
    ~MainThreadQueue();

    ToolResult run(std::function<ToolResult()> task, std::chrono::milliseconds timeout);
    void drain();

private:
    struct Job;
    std::mutex mutex_;
    std::deque<std::shared_ptr<Job>> jobs_;
};

} // namespace mcp
} // namespace wowee
