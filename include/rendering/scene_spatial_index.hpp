#pragma once

#include "rendering/rt_bvh.hpp"
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

/// Producer-owned broad phase. Each placement has one entry, including meshes
/// spanning many cells. Bounds enclose the complete placement, not its centre.
/// Empty cells are removed, so streaming a long route cannot grow the index
/// with the area ever visited. Queries inspect cell bounds before any instances.
class SceneSpatialIndex {
public:
    void clear() { cells_.clear(); entries_.clear(); }

    void remove(uint32_t id) {
        const auto found = entries_.find(id);
        if (found == entries_.end()) return;
        const Entry old = found->second;
        auto cell = cells_.find(old.key);
        auto& ids = cell->second.ids;
        const uint32_t moved = ids.back();
        ids[old.slot] = moved;
        entries_.at(moved).slot = old.slot;
        ids.pop_back();
        entries_.erase(found);
        if (ids.empty()) cells_.erase(cell);
        else cell->second.dirty = true;
    }

    void update(uint32_t id, const RtAabb& bounds) {
        if (!bounds.valid()) { remove(id); return; }
        const uint64_t key = keyFor((bounds.bmin + bounds.bmax) * 0.5f);
        auto found = entries_.find(id);
        if (found != entries_.end() && found->second.key == key) {
            found->second.bounds = bounds;
            cells_.at(key).dirty = true;
            return;
        }
        remove(id);
        auto& cell = cells_[key];
        entries_.emplace(id, Entry{key, static_cast<uint32_t>(cell.ids.size()), bounds});
        cell.ids.push_back(id);
        cell.dirty = true;
    }

    /// The predicate must be conservative on union bounds. Called for cells,
    /// then individual entries, so consumers never read far-away scene records.
    /// The callback must not mutate this index.
    template<class Accept, class Visit>
    void query(Accept&& accept, Visit&& visit) {
        for (auto& [key, cell] : cells_) {
            if (cell.dirty) {
                cell.bounds = {};
                for (uint32_t id : cell.ids) {
                    const auto& b = entries_.at(id).bounds;
                    cell.bounds.grow(b.bmin); cell.bounds.grow(b.bmax);
                }
                cell.dirty = false;
            }
            if (!accept(cell.bounds)) continue;
            for (uint32_t id : cell.ids) {
                const auto& b = entries_.at(id).bounds;
                if (accept(b)) visit(id);
            }
        }
    }

    [[nodiscard]] size_t size() const { return entries_.size(); }
    [[nodiscard]] size_t cells() const { return cells_.size(); }

private:
    static uint64_t keyFor(glm::vec3 p) {
        constexpr float width = 128.0f;
        const auto x = static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.x / width)));
        const auto y = static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.y / width)));
        return (uint64_t(x) << 32) | y;
    }
    struct Entry { uint64_t key; uint32_t slot; RtAabb bounds; };
    struct Cell { RtAabb bounds; std::vector<uint32_t> ids; bool dirty = true; };
    std::unordered_map<uint64_t, Cell> cells_;
    std::unordered_map<uint32_t, Entry> entries_;
};

} // namespace wowee::rendering
