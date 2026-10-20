#pragma once

#include <cstddef>
#include <atomic>
#include <memory>
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
struct SnapshotUse {
    std::atomic<bool> open{true};
    std::atomic<uint64_t> lastFrame{0};
};

/// Kept by a CPU snapshot until it can no longer be recorded. GPU completion
/// alone cannot release resources an unrecorded snapshot still names.
class SnapshotLease {
public:
    explicit SnapshotLease(std::shared_ptr<SnapshotUse> use) : use_(std::move(use)) {}
    ~SnapshotLease() { use_->open.store(false, std::memory_order_release); }
    SnapshotLease(const SnapshotLease&) = delete;
    SnapshotLease& operator=(const SnapshotLease&) = delete;
    void usedByFrame(uint64_t frame) const {
        uint64_t previous = use_->lastFrame.load(std::memory_order_relaxed);
        while (previous < frame && !use_->lastFrame.compare_exchange_weak(previous, frame,
                std::memory_order_release, std::memory_order_relaxed)) {}
    }
private:
    std::shared_ptr<SnapshotUse> use_;
};

class RetireQueue {
public:
    std::shared_ptr<SnapshotLease> holdSnapshot() {
        auto use = std::make_shared<SnapshotUse>();
        std::lock_guard<std::mutex> lock(mutex_);
        snapshots_.emplace_back(use);
        return std::make_shared<SnapshotLease>(std::move(use));
    }

    /// Run `release` once frame `lastUse` and older CPU snapshots have completed.
    void retireAfter(uint64_t lastUse, std::function<void()> release) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry{lastUse, std::move(release), {}};
        auto keep = snapshots_.begin();
        for (auto& weak : snapshots_) {
            if (auto use = weak.lock()) {
                if (use->open.load(std::memory_order_acquire)) {
                    entry.snapshots.push_back(use);
                    *keep++ = weak;
                }
            }
        }
        snapshots_.erase(keep, snapshots_.end());
        entries_.push_back(std::move(entry));
    }

    /// Runs, in the order they were filed, the releases whose last frame is
    /// at or before `completedFrame`. Returns how many ran.
    size_t runCompleted(uint64_t completedFrame, bool deviceIdle = false) {
        std::vector<std::function<void()>> due;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto keep = entries_.begin();
            for (auto& entry : entries_) {
                bool ready = entry.lastUse <= completedFrame;
                for (const auto& use : entry.snapshots) {
                    ready = ready && !use->open.load(std::memory_order_acquire) &&
                            use->lastFrame.load(std::memory_order_acquire) <= completedFrame;
                }
                if (deviceIdle || ready) {
                    due.push_back(std::move(entry.release));
                } else {
                    if (&*keep != &entry) *keep = std::move(entry);
                    ++keep;
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
            const size_t now = runCompleted(UINT64_MAX, true);
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
    struct Entry {
        uint64_t lastUse;
        std::function<void()> release;
        std::vector<std::shared_ptr<SnapshotUse>> snapshots;
    };
    std::vector<Entry> entries_;
    std::vector<std::weak_ptr<SnapshotUse>> snapshots_;
};

}  // namespace wowee::rendering
