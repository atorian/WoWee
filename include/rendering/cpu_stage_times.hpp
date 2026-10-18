#pragma once

#include "core/logger.hpp"
#include "core/profiler.hpp"

#include <array>
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

    /// The start of the renderer's frame.
    void clear() { count_ = 0; }

    void add(std::string_view group, std::string_view name, float milliseconds) {
        if (count_ == entries_.size()) {
            if (!overflowReported_) {
                overflowReported_ = true;
                LOG_WARNING("Renderer stage timing: more than ", entries_.size(),
                            " parts in one frame; '", group, "/", name,
                            "' and the rest of the frame are left out of the frame profile");
            }
            return;
        }
        entries_[count_++] = Entry{group, name, milliseconds};
    }

    [[nodiscard]] std::span<const Entry> entries() const { return {entries_.data(), count_}; }

private:
    std::array<Entry, 64> entries_{};
    size_t count_ = 0;
    bool overflowReported_ = false;
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

} // namespace rendering
} // namespace wowee
