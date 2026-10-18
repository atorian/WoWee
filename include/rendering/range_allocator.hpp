#pragma once

#include <cstdint>
#include <map>
#include <optional>

namespace wowee {
namespace rendering {

/// Hands out ranges of a fixed span of elements - the vertices or indices of
/// one large buffer - and takes them back for reuse.
///
/// Free ranges are kept by offset and merged with their neighbours when one is
/// given back, so a buffer that streams tiles in and out does not fill with
/// slivers. An allocation takes the smallest free range it fits (best fit),
/// which keeps large ranges whole for the chunks that need them.
///
/// Knows nothing of the GPU: a range given back must no longer be read by any
/// frame in flight, which is for the owner to wait out before calling free().
class RangeAllocator {
public:
    explicit RangeAllocator(uint32_t capacity = 0);

    /// The first element of `count` contiguous ones, or nothing when no free
    /// range is that long. A count of zero is never allocated.
    [[nodiscard]] std::optional<uint32_t> allocate(uint32_t count);
    /// Give back a range allocate() returned.
    void free(uint32_t offset, uint32_t count);

    [[nodiscard]] uint32_t capacity() const { return capacity_; }
    [[nodiscard]] uint32_t used() const { return used_; }
    /// The longest range allocate() could return now.
    [[nodiscard]] uint32_t largestFree() const;
    /// How many pieces the free space is in; one when nothing is allocated.
    [[nodiscard]] size_t freeRangeCount() const { return free_.size(); }

private:
    uint32_t capacity_ = 0;
    uint32_t used_ = 0;
    std::map<uint32_t, uint32_t> free_;  // offset -> length
};

} // namespace rendering
} // namespace wowee
