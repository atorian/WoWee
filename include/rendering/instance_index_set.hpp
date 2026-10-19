#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

/// The indices, into an array of instances, of the ones with some property -
/// animated, emitting, lighting - kept right as instances come, go and are
/// swap-removed, at a cost that does not grow with the array.
///
/// The M2 renderer's lists were plain vectors of indices. Taking one instance
/// out meant scanning each list for it, and taking a tile's doodads out meant
/// rebuilding all four over every instance in the world: 6-9 ms of each tile
/// unload with 150,000 of them. Each member here knows its place in the list,
/// so taking it out or following it to a new index is a few stores.
///
/// Iterates as the list of members, in no particular order.
class InstanceIndexSet {
public:
    void insert(size_t instance) {
        if (contains(instance)) return;
        if (instance >= place_.size()) place_.resize(instance + 1, kNone);
        place_[instance] = static_cast<uint32_t>(members_.size());
        members_.push_back(instance);
    }

    void erase(size_t instance) {
        if (!contains(instance)) return;
        const uint32_t at = place_[instance];
        const size_t moved = members_.back();
        members_[at] = moved;
        place_[moved] = at;
        members_.pop_back();
        place_[instance] = kNone;
    }

    /// The instance at `from` now lives at `to`, as a swap-remove moves the
    /// last instance into the place of one taken out. Whatever was at `to`
    /// is gone.
    void move(size_t from, size_t to) {
        erase(to);
        if (!contains(from)) return;
        const uint32_t at = place_[from];
        members_[at] = to;
        if (to >= place_.size()) place_.resize(to + 1, kNone);
        place_[to] = at;
        place_[from] = kNone;
    }

    [[nodiscard]] bool contains(size_t instance) const {
        return instance < place_.size() && place_[instance] != kNone;
    }
    void clear() {
        members_.clear();
        place_.clear();
    }

    [[nodiscard]] size_t size() const { return members_.size(); }
    [[nodiscard]] bool empty() const { return members_.empty(); }
    [[nodiscard]] auto begin() const { return members_.begin(); }
    [[nodiscard]] auto end() const { return members_.end(); }

private:
    static constexpr uint32_t kNone = UINT32_MAX;
    std::vector<size_t> members_;
    /// Each instance's place in members_, kNone for one not in the set.
    std::vector<uint32_t> place_;
};

} // namespace rendering
} // namespace wowee
