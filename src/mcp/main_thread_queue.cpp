#include "mcp/main_thread_queue.hpp"

#include <condition_variable>
#include <exception>

namespace wowee {
namespace mcp {

struct MainThreadQueue::Job {
    enum class State { Waiting, Running, Done, Dropped };
    std::function<ToolResult()> task;
    std::mutex mutex;
    std::condition_variable finished;
    State state = State::Waiting;
    ToolResult result;
};

MainThreadQueue::MainThreadQueue() = default;
MainThreadQueue::~MainThreadQueue() = default;

ToolResult MainThreadQueue::run(std::function<ToolResult()> task, std::chrono::milliseconds timeout) {
    auto job = std::make_shared<Job>();
    job->task = std::move(task);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(job);
    }
    std::unique_lock<std::mutex> lock(job->mutex);
    job->finished.wait_for(lock, timeout, [&] { return job->state == Job::State::Done; });
    if (job->state == Job::State::Waiting) {
        job->state = Job::State::Dropped;
        return ToolResult::error("The client did not reach its next frame in time; nothing was done.");
    }
    // Started before the time ran out: finish it rather than leave it half told.
    job->finished.wait(lock, [&] { return job->state == Job::State::Done; });
    return job->result;
}

void MainThreadQueue::drain() {
    std::deque<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs.swap(jobs_);
    }
    for (auto& job : jobs) {
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            if (job->state != Job::State::Waiting) continue;
            job->state = Job::State::Running;
        }
        ToolResult result;
        try {
            result = job->task();
        } catch (const std::exception& e) {
            result = ToolResult::error(e.what());
        }
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->result = std::move(result);
            job->state = Job::State::Done;
        }
        job->finished.notify_all();
    }
}

} // namespace mcp
} // namespace wowee
