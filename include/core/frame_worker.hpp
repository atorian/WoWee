#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace wowee::core {

/// A single recording owner. Reservations count toward the two-frame limit,
/// so the producer waits before allocating another snapshot, not after it.
/// Only the producer reserves/submits/drains; the consumer never calls back
/// into the producer. Inline mode has exactly the same ownership protocol.
class FrameWorker {
public:
    class Permit {
    public:
        Permit() = default;
        Permit(Permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        Permit& operator=(Permit&& other) noexcept {
            if (this != &other) { release(); owner_ = std::exchange(other.owner_, nullptr); }
            return *this;
        }
        ~Permit() { release(); }
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
    private:
        friend class FrameWorker;
        explicit Permit(FrameWorker* owner) : owner_(owner) {}
        void release() { if (owner_) std::exchange(owner_, nullptr)->completed(); }
        FrameWorker* owner_ = nullptr;
    };

    explicit FrameWorker(bool threaded) : threaded_(threaded) {
        if (threaded_) thread_ = std::thread([this] { run(); });
    }
    ~FrameWorker() { stop(); }
    FrameWorker(const FrameWorker&) = delete;
    FrameWorker& operator=(const FrameWorker&) = delete;

    Permit reserve() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return outstanding_ < 2 || failure_ || stopping_; });
        checkLocked();
        if (stopping_) throw std::logic_error("Render worker is stopped");
        ++outstanding_;
        return Permit(this);
    }
    void submit(Permit permit, std::function<void()> frame) {
        if (permit.owner_ != this) throw std::logic_error("Frame has no render reservation");
        if (!threaded_) {
            { std::lock_guard lock(mutex_); checkLocked(); }
            frame();
            return; // the permit releases after the frame's resources
        }
        {
            std::lock_guard lock(mutex_);
            checkLocked();
            if (stopping_) throw std::logic_error("Render worker is stopped");
            pending_.push_back(std::move(frame));
            permit.owner_ = nullptr; // transferred to the queued job
        }
        changed_.notify_all();
    }
    void check() const { std::lock_guard lock(mutex_); checkLocked(); }
    void drain() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return outstanding_ == 0; });
        checkLocked();
    }
    /// Finish accepted jobs and join even during exception unwinding.
    void stop() noexcept {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
private:
    void checkLocked() const { if (failure_) std::rethrow_exception(failure_); }
    void completed() {
        { std::lock_guard lock(mutex_); --outstanding_; }
        changed_.notify_all();
    }
    void run() noexcept {
        for (;;) {
            std::function<void()> frame;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) return;
                frame = std::move(pending_.front());
                pending_.pop_front();
            }
            try { frame(); }
            catch (...) {
                std::deque<std::function<void()>> cancelled;
                {
                    std::lock_guard lock(mutex_);
                    failure_ = std::current_exception();
                    stopping_ = true;
                    cancelled.swap(pending_);
                    outstanding_ -= cancelled.size();
                }
                cancelled.clear();
            }
            frame = {}; // release the immutable scene before opening a slot
            completed();
        }
    }
    const bool threaded_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> pending_;
    size_t outstanding_ = 0;
    bool stopping_ = false;
    std::exception_ptr failure_;
    std::thread thread_;
};
} // namespace wowee::core
