#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <utility>

namespace wowee::rendering {

/// Reuse CPU snapshot storage only after its last reader has released it.
/// T::clear() releases resources without discarding vector capacity. It runs
/// on the releasing thread and must be noexcept and independent of its owner.
/// Extra retained snapshots allocate normally; only two idle frames are kept.
template<class T>
class FramePool {
public:
    FramePool() : state_(std::make_shared<State>()) {}
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    [[nodiscard]] std::shared_ptr<T> acquire() {
        static_assert(noexcept(std::declval<T&>().clear()));
        std::unique_ptr<T> frame;
        {
            std::lock_guard lock(state_->mutex);
            for (auto& idle : state_->idle) {
                if (idle) { frame = std::move(idle); break; }
            }
        }
        if (!frame) frame = std::make_unique<T>();
        return std::shared_ptr<T>(frame.release(), [state = state_](T* released) noexcept {
            std::unique_ptr<T> owned(released);
            // Model/texture references must die when recording ends, even if
            // the producer never extracts another frame (map change/teardown).
            owned->clear();
            std::lock_guard lock(state->mutex);
            for (auto& idle : state->idle) {
                if (!idle) { idle = std::move(owned); return; }
            }
        });
    }

private:
    struct State {
        std::mutex mutex;
        std::array<std::unique_ptr<T>, 2> idle;
    };
    // The deleter may run after the renderer/pool owner has been destroyed.
    std::shared_ptr<State> state_;
};

} // namespace wowee::rendering
