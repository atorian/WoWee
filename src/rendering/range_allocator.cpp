#include "rendering/range_allocator.hpp"

#include <algorithm>
#include <iterator>

namespace wowee {
namespace rendering {

RangeAllocator::RangeAllocator(uint32_t capacity) : capacity_(capacity) {
    if (capacity_ > 0) free_.emplace(0u, capacity_);
}

std::optional<uint32_t> RangeAllocator::allocate(uint32_t count) {
    if (count == 0) return std::nullopt;
    auto best = free_.end();
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < count) continue;
        if (best == free_.end() || it->second < best->second) {
            best = it;
            if (best->second == count) break;
        }
    }
    if (best == free_.end()) return std::nullopt;
    const uint32_t offset = best->first;
    const uint32_t rest = best->second - count;
    free_.erase(best);
    if (rest > 0) free_.emplace(offset + count, rest);
    used_ += count;
    return offset;
}

void RangeAllocator::free(uint32_t offset, uint32_t count) {
    if (count == 0) return;
    used_ -= std::min(used_, count);
    auto next = free_.lower_bound(offset);
    // Merged into the free range that ends where this one starts...
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            offset = prev->first;
            count += prev->second;
            free_.erase(prev);
        }
    }
    // ...and the one that starts where it ends.
    if (next != free_.end() && offset + count == next->first) {
        count += next->second;
        free_.erase(next);
    }
    free_.emplace(offset, count);
}

uint32_t RangeAllocator::largestFree() const {
    uint32_t largest = 0;
    for (const auto& [offset, length] : free_) largest = std::max(largest, length);
    return largest;
}

} // namespace rendering
} // namespace wowee
