#include "pipeline/client_random.hpp"

#include <bit>
#include <cstring>

namespace wowee {
namespace pipeline {

namespace {

// WoW.exe 12340, 0x9f1700. Read a word at a time at byte offsets that are
// multiples of four, so it is indexed here by offset / 4.
constexpr uint32_t kTable[61] = {
    0x9927148eu, 0x08c7aafdu, 0x1f3ee6d5u, 0xda55bbf6u, 0x6a4aa075u, 0xff97bde8u,
    0x9fbc9bdeu, 0x46a18a81u, 0x63e30b6eu, 0x5d6c7a76u, 0xca69d388u, 0x25b947c3u,
    0x3fa2ab83u, 0xba7c41a6u, 0x0195ace5u, 0xc109cf7eu, 0x717062d9u, 0x0205db8du,
    0x54ef8724u, 0x3037d4c6u, 0x7bcb1bd0u, 0xecd8e4b8u, 0xdcadce49u, 0xc494a913u,
    0x0dae398fu, 0x0edd5218u, 0x85f5fa78u, 0x6dafd258u, 0x3b53b2a4u, 0xbe50a551u,
    0x11f42dfcu, 0xf1169848u, 0x663ddf86u, 0x2f2e445eu, 0x176b0736u, 0xb64c298bu,
    0xe75f89e2u, 0xe121a7cdu, 0xed65c94du, 0x239ceefeu, 0x04b77d33u, 0x402a9a9eu,
    0xf35b10b3u, 0x921c7782u, 0x571e4e20u, 0x8c067222u, 0xfb732c67u, 0xbf0ac259u,
    0x0cf95c79u, 0x68121a28u, 0x42193474u, 0xf884c0b1u, 0x9d15f038u, 0x6f3af260u,
    0x91eb90b4u, 0x61357f1du, 0x5603325au, 0x932bc5a3u, 0x434b0f80u, 0x3ce0a8f7u,
    0x2664d196u,
};

uint32_t word(uint32_t byteOffset) { return kTable[byteOffset / 4]; }

// One tap stepping back by `step` bytes through a ring of `period` bytes.
uint32_t stepBack(uint32_t offset, uint32_t step, uint32_t period) {
    return offset >= step ? offset - step : offset + period - step;
}

} // namespace

void ClientRandom::reseed(uint32_t seed) {
    sum_ = seed;
    taps_ = ((seed % 61u) * 4u) | (((seed % 59u) * 4u) << 8) |
            (((seed % 53u) * 4u) << 16) | (((seed % 47u) * 4u) << 24);
}

uint32_t ClientRandom::next() {
    const uint32_t t3 = stepBack(taps_ >> 24, 0x04, 47 * 4);
    const uint32_t t2 = stepBack((taps_ >> 16) & 0xff, 0x0c, 53 * 4);
    const uint32_t t1 = stepBack((taps_ >> 8) & 0xff, 0x18, 59 * 4);
    const uint32_t t0 = stepBack(taps_ & 0xff, 0x1c, 61 * 4);
    const uint32_t mix = std::rotl(word(t1), 3) ^ std::rotl(word(t2), 2) ^ word(t0) ^
                         std::rotl(word(t3), 1);
    taps_ = (t3 << 24) | (t2 << 16) | (t1 << 8) | t0;
    sum_ += mix;
    return sum_;
}

float ClientRandom::nextSigned() {
    const uint32_t bits = next();
    const uint32_t oneToTwoBits = (bits & 0x7fffffu) | 0x3f800000u;
    float oneToTwo;
    std::memcpy(&oneToTwo, &oneToTwoBits, sizeof(oneToTwo));
    return static_cast<int32_t>(bits) < 0 ? 2.0f - oneToTwo : oneToTwo - 2.0f;
}

} // namespace pipeline
} // namespace wowee
