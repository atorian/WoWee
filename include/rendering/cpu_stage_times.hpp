#pragma once

#include "core/logger.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

namespace wowee {
namespace rendering {

/// What the parts of the renderer's frame cost the CPU, this frame only.
///
/// The client's frame profile timed beginFrame as one number - 8 to 19 ms of
/// a 29 to 60 ms frame in Dalaran - and could not say whether that was the
/// CPU waiting for the GPU's last frame or recording this one. Each part of
/// it is timed here under its own name, and whoever drives the frame reads
/// them back after endFrame: the client into its frame budget, scene_render
/// into its per-frame line.
///
/// Names are held, not copied: string literals, or a frame graph pass's
/// name, which lives until the graph is rebuilt next frame.
class CpuStageTimes {
public:
    struct Entry {
        /// The call or the part it belongs to ("beginFrame", "beginFrame/graph").
        std::string_view group;
        std::string_view name;
        float milliseconds = 0.0f;
    };

    /// The start of the renderer's frame. Not while parts may still be added.
    void clear() { count_.store(0, std::memory_order_relaxed); }

    /// Safe from several threads at once: each part claims its own slot. Read
    /// the entries only after every thread that adds has been joined.
    void add(std::string_view group, std::string_view name, float milliseconds) {
        const size_t slot = count_.fetch_add(1, std::memory_order_relaxed);
        if (slot >= entries_.size()) {
            count_.store(entries_.size(), std::memory_order_relaxed);
            if (!overflowReported_.exchange(true)) {
                LOG_WARNING("Renderer stage timing: more than ", entries_.size(),
                            " parts in one frame; '", group, "/", name,
                            "' and the rest of the frame are left out of the frame profile");
            }
            return;
        }
        entries_[slot] = Entry{group, name, milliseconds};
    }

    [[nodiscard]] std::span<const Entry> entries() const {
        return {entries_.data(), std::min(count_.load(std::memory_order_relaxed), entries_.size())};
    }

private:
    std::array<Entry, 64> entries_{};
    std::atomic<size_t> count_{0};
    std::atomic<bool> overflowReported_{false};
};

/// Times what runs between its construction and its end as one part, and
/// in a Tracy build is a CPU zone of the same name.
class CpuStage {
public:
    CpuStage(CpuStageTimes& times, std::string_view group, std::string_view name)
        : times_(times), group_(group), name_(name),
#ifdef TRACY_ENABLE
          zone_(__LINE__, __FILE__, sizeof(__FILE__) - 1, "CpuStage", sizeof("CpuStage") - 1,
                name.data(), name.size(), true),
#endif
          started_(std::chrono::steady_clock::now()) {}

    ~CpuStage() {
        times_.add(group_, name_, std::chrono::duration<float, std::milli>(
                                      std::chrono::steady_clock::now() - started_).count());
    }

    CpuStage(const CpuStage&) = delete;
    CpuStage& operator=(const CpuStage&) = delete;

private:
    CpuStageTimes& times_;
    std::string_view group_;
    std::string_view name_;
#ifdef TRACY_ENABLE
    tracy::ScopedZone zone_;
#endif
    std::chrono::steady_clock::time_point started_;
};

/// Splits one long stretch of code into consecutive parts: each lap() records
/// the time since the previous one (or since construction) under its name.
/// For a function that is a list of steps, where wrapping each step in a scope
/// would reindent the whole of it. Records nothing without somewhere to put it.
class CpuStageLaps {
public:
    CpuStageLaps(CpuStageTimes* times, std::string_view group)
        : times_(times), group_(group), last_(std::chrono::steady_clock::now()) {}

    void lap(std::string_view name) {
        if (!times_) return;
        const auto now = std::chrono::steady_clock::now();
        times_->add(group_, name, std::chrono::duration<float, std::milli>(now - last_).count());
        last_ = now;
    }

private:
    CpuStageTimes* times_;
    std::string_view group_;
    std::chrono::steady_clock::time_point last_;
};

} // namespace rendering
} // namespace wowee
