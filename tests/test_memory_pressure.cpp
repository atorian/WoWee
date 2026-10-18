// How short of memory the machine is said to be, for the free RAM it has. The
// terrain workers slow down at the first level and wait longer at the second,
// so the second must be the stricter: a machine severely short is short.
#include <catch_amalgamated.hpp>

#include "core/memory_monitor.hpp"

#include <cstddef>

using wowee::core::MemoryMonitor;
using wowee::core::MemoryPressure;

namespace {
constexpr size_t kTotal = 16ull * 1024 * 1024 * 1024;
constexpr size_t percentFree(size_t percent) { return kTotal / 100 * percent; }
} // namespace

TEST_CASE("plenty of free memory is no pressure", "[memory]") {
    CHECK(MemoryMonitor::classify(percentFree(50), kTotal) == MemoryPressure::None);
    CHECK(MemoryMonitor::classify(percentFree(15), kTotal) == MemoryPressure::None);
}

TEST_CASE("a little short of memory is low, not severe", "[memory]") {
    CHECK(MemoryMonitor::classify(percentFree(12), kTotal) == MemoryPressure::Low);
    CHECK(MemoryMonitor::classify(percentFree(10), kTotal) == MemoryPressure::Low);
}

TEST_CASE("very short of memory is severe", "[memory]") {
    CHECK(MemoryMonitor::classify(percentFree(9), kTotal) == MemoryPressure::Severe);
    CHECK(MemoryMonitor::classify(0, kTotal) == MemoryPressure::Severe);
}
