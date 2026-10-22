#pragma once
#include <atomic>
#include <cstdint>

namespace wowee::rendering {
// One namespace across model families and renderer instances; never recycled.
inline uint64_t nextSurfaceIdentity(uint64_t count=1) {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(count,std::memory_order_relaxed);
}
}
