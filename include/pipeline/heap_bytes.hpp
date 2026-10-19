#pragma once

// The heap memory a container holds, for the resource line's account of what
// the client keeps in CPU memory. Capacity, not size: a vector that grew and
// was cleared still holds what it grew to.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline {

template <class T>
[[nodiscard]] uint64_t vectorBytes(const std::vector<T>& v) {
    return static_cast<uint64_t>(v.capacity()) * sizeof(T);
}

/// A string's own allocation; nothing while it fits the inline buffer.
[[nodiscard]] inline uint64_t stringBytes(const std::string& s) {
    return s.capacity() > std::string().capacity() ? static_cast<uint64_t>(s.capacity()) + 1 : 0;
}

/// A vector and what each of its elements holds, by `inner(element)`.
template <class T, class Inner>
[[nodiscard]] uint64_t vectorBytes(const std::vector<T>& v, Inner&& inner) {
    uint64_t total = vectorBytes(v);
    for (const T& e : v) total += inner(e);
    return total;
}

} // namespace pipeline
} // namespace wowee
