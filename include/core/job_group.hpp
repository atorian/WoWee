#pragma once

#include "core/thread_pool.hpp"
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace wowee::core {

/// A lifetime fence for work that may outlive the frame that submitted it.
/// Workers receive a token and check it before I/O and between expensive steps.
/// Consumers check the same token before using a result. Cancellation is normal,
/// not an asset failure. Destruction cancels and joins before the owner disappears.
class JobGroup {
    struct State {
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        std::condition_variable done;
        size_t running = 0;
    };
public:
    class Token {
    public:
        bool current() const {
            return state_ && !state_->cancelled.load(std::memory_order_acquire) &&
                   (!request_ || request_->load(std::memory_order_acquire));
        }
        void cancel() const {
            if (request_) request_->store(false, std::memory_order_release);
            else if (state_) state_->cancelled.store(true, std::memory_order_release);
        }
    private:
        friend class JobGroup;
        std::shared_ptr<State> state_;
        std::shared_ptr<std::atomic<bool>> request_;
    };

    class Lease {
    public:
        Lease() = default;
        Lease(Lease&& other) noexcept : state_(std::move(other.state_)) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            if (!state_) return;
            std::lock_guard lock(state_->mutex);
            if (--state_->running == 0) state_->done.notify_all();
        }
        explicit operator bool() const { return bool(state_); }
    private:
        friend class JobGroup;
        explicit Lease(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    template<class T> struct Future {
        std::future<T> value;
        Token token;
        bool valid() const { return value.valid(); }
        bool current() const { return token.current(); }
        void wait() const { value.wait(); }
        template<class Rep, class Period>
        auto wait_for(std::chrono::duration<Rep, Period> duration) const { return value.wait_for(duration); }
        T get() { return value.get(); }
    };

    enum class Channel { Creature, Player, Equipment, GameObject };

    JobGroup() = default;
    JobGroup(const JobGroup&) = delete;
    JobGroup& operator=(const JobGroup&) = delete;
    ~JobGroup() { cancel(); wait(); }

    Token token() const { Token t; t.state_ = state_; return t; }
    bool accepts(const Token& t) const { return t.state_ == state_ && t.current(); }

    /// Only called on the owning thread. A new request invalidates every queued
    /// or completed result of the previous request, including reuse of a GUID.
    Token replace(Channel channel, uint64_t guid) {
        auto& t = requests_[{channel, guid}];
        t.cancel();
        t = token();
        t.request_ = std::make_shared<std::atomic<bool>>(true);
        return t;
    }
    Token request(Channel channel, uint64_t guid) {
        if (auto it = requests_.find({channel, guid}); it != requests_.end()) return it->second;
        return replace(channel, guid);
    }
    void cancel(Channel channel, uint64_t guid) {
        if (auto it = requests_.find({channel, guid}); it != requests_.end()) {
            it->second.cancel();
            requests_.erase(it);
        }
    }
    void cancel() { state_->cancelled.store(true, std::memory_order_release); }
    void wait() const {
        std::unique_lock lock(state_->mutex);
        state_->done.wait(lock, [&] { return state_->running == 0; });
    }

    /// For persistent worker loops as well as pooled tasks. Acquire before the
    /// task becomes visible to a worker; release after its final owner access.
    static Lease track(const Token& t) {
        if (!t.state_) return {};
        std::lock_guard lock(t.state_->mutex);
        if (!t.current()) return {};
        ++t.state_->running;
        return Lease(t.state_);
    }
    template<class F>
    auto submit(Token t, F&& fn) {
        using R = std::invoke_result_t<std::decay_t<F>, const Token&>;
        auto lease = track(t);
        auto future = ThreadPool::ioWorkers().submit(
            [t, lease = std::move(lease), fn = std::forward<F>(fn)]() mutable {
                // packaged_task can keep its callable alive until get(). The
                // fence ends when work returns, not when its future is consumed.
                auto running = std::move(lease);
                return fn(t);
            });
        return Future<R>{std::move(future), std::move(t)};
    }
    template<class F> auto submit(F&& fn) { return submit(token(), std::forward<F>(fn)); }

private:
    struct Key {
        Channel channel;
        uint64_t guid;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        size_t operator()(const Key& k) const {
            return std::hash<uint64_t>{}(k.guid) ^ (static_cast<size_t>(k.channel) << 1);
        }
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::unordered_map<Key, Token, Hash> requests_;
};

} // namespace wowee::core
