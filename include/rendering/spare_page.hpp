#pragma once

// One pool page made ahead on a worker, so the frame that needs a new page
// takes it rather than allocating GPU memory itself - a new memory block costs
// the thread that asks for it 7-30 ms on Windows (AGENTS.md, "Loading in the
// world never holds a frame").
//
// Deterministic: a page is made exactly when one is taken, never on a timer
// or a guess at what is free. The first is asked for with the pool's first
// page, which the frame makes itself - a pool never used holds no spare. When
// the frame needs a page before the next is made it makes its own, and the
// pool says so.

#include "foundation/thread_pool.hpp"

#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace wowee::rendering {

template <class Page>
class SparePage {
public:
    /// Makes a page; on a worker. Null when there is no memory for one, said
    /// by whoever takes the spare and finds none.
    using Make = std::function<std::shared_ptr<Page>()>;
    /// Gives back a page never handed out: no frame has used it.
    using Destroy = std::function<void(Page&)>;

    SparePage() = default;
    ~SparePage() { stop(); }
    SparePage(const SparePage&) = delete;
    SparePage& operator=(const SparePage&) = delete;

    /// How pages are made; none is until the first take().
    void start(Make make, Destroy destroy) {
        stop();
        make_ = std::move(make);
        destroy_ = std::move(destroy);
        state_ = std::make_shared<State>();
    }

    /// The spare, and the next one started; null the first time (which
    /// starts the first), while it is still being made, and when it could
    /// not be.
    [[nodiscard]] std::shared_ptr<Page> take() {
        if (!state_) return nullptr;
        std::lock_guard lock(state_->mutex);
        if (state_->making) return nullptr;
        std::shared_ptr<Page> page = std::move(state_->page);
        begin();
        return page;
    }

    /// Whether a page has ever been asked for.
    [[nodiscard]] bool started() const { return work_.valid(); }

    /// Waits for a page being made and gives the spare back.
    void stop() {
        if (!state_) return;
        if (work_.valid()) work_.wait();
        work_ = {};
        if (state_->page) destroy_(*state_->page);
        state_.reset();
    }

private:
    struct State {
        std::mutex mutex;
        std::shared_ptr<Page> page;
        bool making = false;
    };
    // With the state's lock held.
    void begin() {
        state_->making = true;
        work_ = core::ThreadPool::ioWorkers().submit("spare pool page", [state = state_, make = make_] {
            std::shared_ptr<Page> page = make();
            std::lock_guard lock(state->mutex);
            state->page = std::move(page);
            state->making = false;
        });
    }

    Make make_;
    Destroy destroy_;
    std::shared_ptr<State> state_;
    std::future<void> work_;
};

}  // namespace wowee::rendering
