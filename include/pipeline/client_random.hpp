#pragma once

#include <cstdint>

namespace wowee {
namespace pipeline {

/// The seeded random number generator of the 3.3.5a client (WoW.exe 12340:
/// seed 0x4c1510, next 0x464580, its table at 0x9f1700).
///
/// Ported rather than replaced because what the client scatters with it is
/// laid out by it: the ground clutter of a chunk is a function of this
/// sequence and the chunk's seed, so the same generator puts every tuft where
/// the original puts it.
///
/// Four lagged taps walk one table of 61 words at the periods 47, 53, 59 and
/// 61; each draw adds the mix of the four to a running sum and returns it.
class ClientRandom {
public:
    explicit ClientRandom(uint32_t seed) { reseed(seed); }

    void reseed(uint32_t seed);

    /// The next 32 bits.
    uint32_t next();

    /// The next draw as a float in [-1, 1], made the client's way: the low 23
    /// bits as the mantissa of a number in [1, 2), mirrored by the sign bit.
    float nextSigned();

private:
    uint32_t sum_ = 0;
    uint32_t taps_ = 0;  // four byte offsets into the table, one per tap
};

} // namespace pipeline
} // namespace wowee
