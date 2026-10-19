#pragma once

// Keeping a list of indices into an array right across a swap-remove.
//
// The M2 renderer removes an instance by moving the last one into its slot
// and popping the end, and keeps lists of indices into that array (the
// animated, the emitting, the lit). Rebuilding those lists after a removal
// walks every instance - 110,000 to 200,000 of them in a city - and a
// despawn of a few dozen game objects removed them one at a time, 12-43 ms
// in one frame. Correcting each list for the one move costs a pass over the
// list alone.

#include <cstddef>
#include <vector>

namespace wowee {
namespace rendering {

/// The element at `removed` is gone, and the one that was at `last` - the
/// array's final slot before the removal - now sits at `removed`. Drops
/// `removed` from `list` and renames `last` to `removed`. Order is not kept.
inline void swapRemoveIndex(std::vector<size_t>& list, size_t removed, size_t last) {
    for (size_t k = 0; k < list.size();) {
        if (list[k] == removed) {
            list[k] = list.back();
            list.pop_back();
        } else {
            ++k;
        }
    }
    if (last == removed) return;
    for (size_t& v : list) {
        if (v == last) v = removed;
    }
}

} // namespace rendering
} // namespace wowee
