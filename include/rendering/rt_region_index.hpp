#pragma once

// Where each of RtScene's source instances stands, gathered in columns of the
// map, so that when the region the ray tracer keeps moves, only the instances
// of the columns it can reach are tested. A column's bounds hold all its
// members' bounds, and RtRegion::keeps admits no less of a larger box, so a
// column it turns away holds no instance it would admit. Testing every
// instance instead cost 1.9 ms of a Dalaran frame, each frame the camera
// moved.

#include "rendering/rt_region.hpp"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

class RtRegionIndex {
public:
    /// An instance is placed with these bounds, or has moved to them.
    void place(uint32_t id, const RtAabb& bounds) {
        if (id >= slots_.size()) slots_.resize(static_cast<size_t>(id) + 1);
        const uint64_t key = keyOf(bounds);
        if (slots_[id].placed && slots_[id].column == key) {
            // What it left behind may still hold the column's bounds out.
            Column& column = columns_.at(key);
            column.bounds.grow(bounds);
            column.loose = true;
            return;
        }
        remove(id);
        Column& column = columns_[key];
        slots_[id] = {.column = key, .index = static_cast<uint32_t>(column.members.size()), .placed = true};
        column.members.push_back(id);
        column.bounds.grow(bounds);
    }

    void remove(uint32_t id) {
        if (id >= slots_.size() || !slots_[id].placed) return;
        Slot& slot = slots_[id];
        auto it = columns_.find(slot.column);
        auto& members = it->second.members;
        const uint32_t last = members.back();
        members[slot.index] = last;
        slots_[last].index = slot.index;
        members.pop_back();
        if (members.empty()) columns_.erase(it);
        else it->second.loose = true;
        slot = {};
    }

    /// Visits every instance whose bounds the region may admit as a new
    /// arrival - without the margin it gives what it already keeps - and
    /// some that it will not. `boundsOf(id)` is an instance's bounds now,
    /// read to draw a column's bounds in once members have left or moved.
    template <class BoundsOf, class Visit>
    void forEachReachable(const RtRegion& region, BoundsOf&& boundsOf, Visit&& visit) {
        for (auto& [key, column] : columns_) {
            if (column.loose) {
                column.bounds = {};
                for (uint32_t id : column.members) column.bounds.grow(boundsOf(id));
                column.loose = false;
            }
            if (!region.keeps(column.bounds, false)) continue;
            for (uint32_t id : column.members) visit(id);
        }
    }

private:
    /// Columns are a quarter of an ADT tile across.
    static constexpr float kColumn = 533.33333f / 4.0f;

    struct Column {
        RtAabb bounds;
        std::vector<uint32_t> members;
        bool loose = false;
    };
    struct Slot {
        uint64_t column = 0;
        uint32_t index = 0;
        bool placed = false;
    };

    static uint64_t keyOf(const RtAabb& bounds) {
        const glm::vec3 centre = (bounds.bmin + bounds.bmax) * 0.5f;
        const auto x = static_cast<int32_t>(std::floor(centre.x / kColumn));
        const auto y = static_cast<int32_t>(std::floor(centre.y / kColumn));
        return (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
    }

    std::unordered_map<uint64_t, Column> columns_;
    std::vector<Slot> slots_;
};

} // namespace wowee::rendering
