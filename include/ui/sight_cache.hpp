#pragma once

// Whether a wall stands between the camera and something in the world.
//
// The nameplates asked this first: a name that reads through a wall puts a
// player in a room they are not in. Chat bubbles have exactly the same fault
// and the same answer, so the query lives here rather than in one of them,
// where the second copy would have drifted from the first.
//
// It is a collision query against the WMO geometry, so it is remembered per
// subject rather than run every frame for everything on screen.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace wowee {
namespace ui {

/// Per-subject line of sight, found a few subjects a frame.
///
/// An answer goes stale when either end moves - and the near end is the
/// camera, which moves whenever the player walks or turns. Refreshing every
/// stale answer as it was asked for put every plate's query in the same frame:
/// a crowd in Dalaran, one collision query against the city each, 4.7 ms at
/// p95 for the nameplates alone. So blocked() only answers from what is known
/// and lists what is stale, and refresh() runs the queries, oldest first,
/// within a count and a time per frame.
///
/// A subject never asked about before has no answer yet and counts as out of
/// sight until it has one, first in the line: a plate that appears a frame or
/// two late is not noticed, one drawn through a wall is.
class SightCache {
public:
    /// True when something solid stands between `from` and `to`, as last
    /// found, or when nothing has been found for this subject yet.
    ///
    /// `key` is whatever identifies the subject to the caller - a guid, in
    /// both of the places this is used. Each caller keeps its own cache, so
    /// two views of the same unit at different heights do not fight over one
    /// entry.
    bool blocked(uint64_t key, const glm::vec3& from, const glm::vec3& to) {
        Entry& e = entries_[key];
        e.wantFrom = from;
        e.wantTo = to;
        e.askedFrame = frame_;
        if (e.known) {
            ++e.age;
            // Stale when either end has moved far enough to change the
            // answer, or after a while regardless, since the geometry
            // between them streams in and out.
            const bool moved = glm::distance(e.from, from) > kMovedUnits ||
                               glm::distance(e.to, to) > kMovedUnits;
            if (!moved && e.age < kMaxAgeFrames) return e.blocked;
        }
        if (!e.queued) {
            e.queued = true;
            stale_.push_back(key);
        }
        return !e.known || e.blocked;
    }

    /// Run `query(from, to)` - the collision test itself - for the subjects
    /// waiting on an answer: the unanswered first, then the oldest, until
    /// kRefreshesPerFrame have run or kRefreshBudgetMs has gone. Once a frame,
    /// after the asking. The rest wait and grow older, so none is left behind
    /// for long.
    template <class Query>
    void refresh(Query&& query) {
        ++frame_;
        forgetUnasked();
        if (stale_.empty()) return;
        std::sort(stale_.begin(), stale_.end(), [this](uint64_t a, uint64_t b) {
            return priority(a) > priority(b);
        });
        const auto start = std::chrono::steady_clock::now();
        std::size_t done = 0;
        while (done < stale_.size() && done < kRefreshesPerFrame) {
            // At least one a frame, however slow, or a city of slow queries
            // would never answer anything.
            if (done > 0 && std::chrono::duration<float, std::milli>(
                                std::chrono::steady_clock::now() - start).count() >= kRefreshBudgetMs) {
                break;
            }
            auto it = entries_.find(stale_[done++]);
            if (it == entries_.end()) continue;
            Entry& e = it->second;
            e.queued = false;
            e.known = true;
            e.from = e.wantFrom;
            e.to = e.wantTo;
            e.age = 0;
            e.blocked = query(e.wantFrom, e.wantTo);
        }
        stale_.erase(stale_.begin(), stale_.begin() + static_cast<std::ptrdiff_t>(done));
    }

    static constexpr std::size_t kRefreshesPerFrame = 8;
    static constexpr float kRefreshBudgetMs = 0.5f;
    static constexpr int kMaxAgeFrames = 15;

private:
    /// Units walk in and out of range all evening; kept for every one ever
    /// seen, the map would grow for the life of the process. Room for a
    /// crowded city - Dalaran holds 600 characters and its NPCs - so a unit
    /// that turns back into view finds its last answer rather than waiting in
    /// line for one. An entry is some 80 bytes.
    static constexpr std::size_t kMaxEntries = 1024;
    static constexpr float kMovedUnits = 0.5f;

    struct Entry {
        /// The ends the answer was found for.
        glm::vec3 from{};
        glm::vec3 to{};
        /// The ends last asked about, which a refresh finds the answer for.
        glm::vec3 wantFrom{};
        glm::vec3 wantTo{};
        uint64_t askedFrame = 0;
        int age = 0;
        bool known = false;
        bool queued = false;
        bool blocked = false;
    };

    /// Unanswered before answered, then oldest first.
    [[nodiscard]] long long priority(uint64_t key) const {
        auto it = entries_.find(key);
        if (it == entries_.end()) return -1;
        return it->second.known ? it->second.age : (1LL << 40);
    }

    /// Past the bound, the subjects nobody asked about last frame go - the
    /// units off screen or out of range. One that comes back is answered
    /// afresh. Only those: dropping everything, as this once did, put every
    /// plate on screen back in the line at once.
    void forgetUnasked() {
        if (entries_.size() <= kMaxEntries) return;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.askedFrame + 1 < frame_) it = entries_.erase(it);
            else ++it;
        }
    }

    std::unordered_map<uint64_t, Entry> entries_;
    std::vector<uint64_t> stale_;
    uint64_t frame_ = 1;
};

}  // namespace ui
}  // namespace wowee
