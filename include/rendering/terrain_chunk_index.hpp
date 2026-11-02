#pragma once

// Where each resident terrain chunk is, by its place in TerrainRenderer's
// list of chunks: found by its place on the map, linked to the chunks beside
// it, and marked in the coverage the distant terrain leaves to it. The list
// keeps no order - a leaving chunk is filled by the last - so the index moves
// with it, and every frame reads the links instead of looking the neighbours
// up again (four hash lookups for each of ~5,400 chunks drawn, 0.6-0.8 ms of
// a Dalaran frame).

#include "pipeline/distant_terrain.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

class TerrainChunkIndex {
public:
    /// No chunk.
    static constexpr uint32_t kNone = UINT32_MAX;
    /// A chunk's edges, as its skirts are ordered: toward y-1, y+1, x-1, x+1.
    static constexpr size_t kEdges = 4;

    /// A chunk at map chunk (gx, gy) - tile * 16 + chunk along each axis -
    /// appended to the list; its list place, and its neighbours linked both
    /// ways.
    uint32_t add(int gx, int gy) {
        const auto index = static_cast<uint32_t>(entries_.size());
        Entry entry{.gx = gx, .gy = gy};
        for (size_t edge = 0; edge < kEdges; ++edge) {
            const uint32_t beside = find(gx + kStep[edge][0], gy + kStep[edge][1]);
            entry.neighbours[edge] = beside;
            if (beside != kNone) entries_[beside].neighbours[opposite(edge)] = index;
        }
        entries_.push_back(entry);
        at_[keyOf(gx, gy)] = index;
        mark(gx, gy, true);
        return index;
    }

    /// The chunk at list place `index` gone, and the last of the list moved
    /// into its place, as the renderer moves its own.
    void removeSwapLast(uint32_t index) {
        const Entry leaving = entries_[index];
        for (size_t edge = 0; edge < kEdges; ++edge) {
            if (leaving.neighbours[edge] != kNone) entries_[leaving.neighbours[edge]].neighbours[opposite(edge)] = kNone;
        }
        at_.erase(keyOf(leaving.gx, leaving.gy));
        mark(leaving.gx, leaving.gy, false);
        const auto last = static_cast<uint32_t>(entries_.size() - 1);
        if (index != last) {
            const Entry& moved = entries_[index] = entries_[last];
            at_[keyOf(moved.gx, moved.gy)] = index;
            for (size_t edge = 0; edge < kEdges; ++edge) {
                if (moved.neighbours[edge] != kNone) entries_[moved.neighbours[edge]].neighbours[opposite(edge)] = index;
            }
        }
        entries_.pop_back();
    }

    void clear() {
        entries_.clear();
        at_.clear();
        coverage_ = {};
    }

    [[nodiscard]] size_t size() const { return entries_.size(); }
    /// The list place of the chunk at map chunk (gx, gy), kNone for none.
    [[nodiscard]] uint32_t find(int gx, int gy) const {
        const auto it = at_.find(keyOf(gx, gy));
        return it == at_.end() ? kNone : it->second;
    }
    /// The list place of the chunk beside `index` across `edge`, kNone for
    /// none.
    [[nodiscard]] uint32_t neighbour(uint32_t index, size_t edge) const { return entries_[index].neighbours[edge]; }
    /// Each tile's resident chunks, a bit per chunk (row-major in the tile).
    [[nodiscard]] const pipeline::TerrainCoverage& coverage() const { return coverage_; }

private:
    struct Entry {
        int gx = 0, gy = 0;
        std::array<uint32_t, kEdges> neighbours{kNone, kNone, kNone, kNone};
    };
    static constexpr int kStep[kEdges][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    static constexpr size_t opposite(size_t edge) { return edge ^ 1u; }
    static uint64_t keyOf(int gx, int gy) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(gx)) << 32) | static_cast<uint32_t>(gy);
    }
    void mark(int gx, int gy, bool resident) {
        if (gx < 0 || gy < 0 || gx >= 64 * 16 || gy >= 64 * 16) return;
        const int cell = (gy % 16) * 16 + gx % 16;
        uint32_t& word = coverage_[static_cast<size_t>((gy / 16) * 64 + gx / 16)][static_cast<size_t>(cell / 32)];
        const uint32_t bit = 1u << (cell % 32);
        word = resident ? (word | bit) : (word & ~bit);
    }

    std::vector<Entry> entries_;
    std::unordered_map<uint64_t, uint32_t> at_;
    pipeline::TerrainCoverage coverage_{};
};

}  // namespace wowee::rendering
