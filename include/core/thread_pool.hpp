#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace wowee {
namespace core {

// Persistent worker pool for short-lived parallel jobs that run every frame
// (bone matrix computation, secondary command buffer recording, collision
// floor queries). Replaces per-frame std::async calls, which create and
// destroy an OS thread per invocation on libstdc++/libc++ - hundreds of
// thread spawns per second at typical frame rates.
//
// Tasks submitted to the pool must not block waiting on other pool tasks,
// with one sanctioned exception: a task may submit chunked sub-work and wait
// for it, provided it also processes a share of that work on its own thread
// (caller-runs-a-chunk pattern). All other tasks are non-blocking compute,
// so the queue always drains and such waits always complete.
class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount) {
        workers_.reserve(threadCount);
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Shared pool for per-frame render/update work, sized for the machine.
    static ThreadPool& frameWorkers() {
        static ThreadPool pool(defaultThreadCount());
        return pool;
    }

    // Small, separate pool for blocking filesystem work. Keeping it distinct from
    // frameWorkers prevents a slow disk read from occupying render/update workers.
    static ThreadPool& ioWorkers() {
        static ThreadPool pool(2);
        return pool;
    }

    // Schedule fn on a worker thread. The returned future carries fn's result
    // or any exception it threw (same semantics as std::async). `label` names
    // the kind of work in the pool's timings; a string literal.
    template <typename F>
    auto submit(const char* label, F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(Queued{[task]() { (*task)(); }, label, std::chrono::steady_clock::now()});
        }
        cv_.notify_one();
        return fut;
    }
    template <typename F>
    auto submit(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        return submit("unlabelled", std::forward<F>(fn));
    }

    [[nodiscard]] size_t threadCount() const { return workers_.size(); }

    /// What one task cost: how long it sat in the queue before a worker took
    /// it, and how long it ran.
    struct TaskTiming {
        const char* label = nullptr;
        float queuedMs = 0.0f;
        float runMs = 0.0f;
    };
    /// Record every task's timing from now on, for the frame profile; off,
    /// tasks cost nothing extra.
    void setTiming(bool on) { timing_.store(on, std::memory_order_relaxed); }
    /// The timings of the tasks finished since the last call.
    [[nodiscard]] std::vector<TaskTiming> takeTimings() {
        std::lock_guard<std::mutex> lock(timingsMutex_);
        std::vector<TaskTiming> out;
        out.swap(timings_);
        return out;
    }

private:
    static size_t defaultThreadCount() {
        size_t hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        // Leave a core for the main thread; cap for very wide machines.
        size_t count = (hw > 1) ? hw - 1 : 1;
        if (count < 2) count = 2;
        if (count > 16) count = 16;
        return count;
    }

    struct Queued {
        std::function<void()> fn;
        const char* label = nullptr;
        std::chrono::steady_clock::time_point submitted;
    };

    void workerLoop() {
        for (;;) {
            Queued task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            if (!timing_.load(std::memory_order_relaxed)) {
                task.fn();
                continue;
            }
            const auto started = std::chrono::steady_clock::now();
            task.fn();
            const auto finished = std::chrono::steady_clock::now();
            using Ms = std::chrono::duration<float, std::milli>;
            std::lock_guard<std::mutex> lock(timingsMutex_);
            timings_.push_back({task.label, Ms(started - task.submitted).count(),
                                Ms(finished - started).count()});
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Queued> tasks_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
    std::atomic<bool> timing_{false};
    std::mutex timingsMutex_;
    std::vector<TaskTiming> timings_;
};

} // namespace core
} // namespace wowee
