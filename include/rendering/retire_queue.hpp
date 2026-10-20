#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace wowee::rendering {

/// Releases held back until the GPU has finished every frame that could use
/// what they release.
///
/// Frames are numbered in the order they are submitted. A release is filed
/// under the last frame that may still reference the object - a buffer a
/// command buffer binds, a descriptor set one draws with - and runs once that
/// frame has completed.
///
/// It used to be a list per frame slot, run when the slot came round again.
/// That ties a release to "the frame being built when it was asked for", which
/// holds only while loading and recording take turns on one thread: a frame
/// recorded later from a snapshot taken earlier can still use an object the
/// game has let go of since. A frame number says which frame that is, and
/// whoever records it says when it has completed.
///
/// Releases are filed from any thread; they run on the thread that reports a
/// completed frame (or flushes), outside the lock, so a release may file
/// another.
class RetireQueue {
public:
    /// Run `release` once frame `lastUse` has completed.
    void retireAfter(uint64_t lastUse, std::function<void()> release) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.emplace_back(lastUse, std::move(release));
    }

    /// Runs, in the order they were filed, the releases whose last frame is
    /// at or before `completedFrame`. Returns how many ran.
    size_t runCompleted(uint64_t completedFrame) {
        std::vector<std::function<void()>> due;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto keep = entries_.begin();
            for (auto& entry : entries_) {
                if (entry.first <= completedFrame) {
                    due.push_back(std::move(entry.second));
                } else {
                    *keep++ = std::move(entry);
                }
            }
            entries_.erase(keep, entries_.end());
        }
        for (auto& release : due) {
            if (release) release();
        }
        return due.size();
    }

    /// Runs everything, including what the releases file while running: for
    /// when no frame is in flight any more (shutdown, the device idle).
    size_t runAll() {
        size_t ran = 0;
        for (;;) {
            const size_t now = runCompleted(UINT64_MAX);
            if (now == 0) return ran;
            ran += now;
        }
    }

    [[nodiscard]] size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<uint64_t, std::function<void()>>> entries_;
};

}  // namespace wowee::rendering
