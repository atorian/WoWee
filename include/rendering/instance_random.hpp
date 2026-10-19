#pragma once

// The random numbers one model instance draws - particle spawn jitter, the
// next idle variation, where its animation starts - from a generator of its
// own.
//
// The M2 renderer drew them from one generator for the whole scene, so no two
// instances could be updated at once: they would race on its state. Each
// instance carrying eight bytes of state, seeded from its id, makes an
// instance's draws depend on that instance alone, on whichever thread it is
// updated and in whatever order.

#include <cstdint>

namespace wowee {
namespace rendering {

/// xorshift64*, seeded through splitmix64. A UniformRandomBitGenerator, so
/// the standard distributions take it.
class InstanceRandom {
public:
    using result_type = uint32_t;

    InstanceRandom() = default;
    explicit InstanceRandom(uint64_t seed) : state_(mix(seed)) {}

    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return 0xffffffffu; }

    result_type operator()() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return static_cast<result_type>((state_ * 0x2545F4914F6CDD1DULL) >> 32);
    }

    /// Uniform in [lo, hi), from the top 24 bits.
    float uniform(float lo, float hi) {
        return lo + (hi - lo) * static_cast<float>((*this)() >> 8) * (1.0f / 16777216.0f);
    }

    /// Uniform in [0, n); zero for n of zero.
    uint32_t below(uint32_t n) {
        return n == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>((*this)()) * n) >> 32);
    }

private:
    /// splitmix64: spreads a small seed such as an instance id over all 64
    /// bits, and never answers zero, which xorshift cannot leave.
    static uint64_t mix(uint64_t z) {
        z += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return z ? z : 0x9E3779B97F4A7C15ULL;
    }

    uint64_t state_ = 0x9E3779B97F4A7C15ULL;
};

} // namespace rendering
} // namespace wowee
