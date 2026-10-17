// Normal/height maps go to the GPU as BC7: a quarter of the RGBA8 they were,
// with every channel - the normal's three and the height - close to what was
// derived. The blocks are read back with a mode 6 decoder written from the
// format's definition, so what is checked is what the GPU will sample.
#include <catch_amalgamated.hpp>

#include "rendering/normal_map_encoding.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using wowee::rendering::EncodedNormalMap;
using wowee::rendering::encodeBc7Block;
using wowee::rendering::encodeNormalMap;
using wowee::rendering::NormalMapFormat;

namespace {

uint32_t bits(const std::array<uint8_t, 16>& block, int& at, int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i, ++at) {
        value |= static_cast<uint32_t>((block[at / 8] >> (at % 8)) & 1u) << i;
    }
    return value;
}

/// BC7 mode 6: one subset, 7-bit RGBA endpoints with a parity bit each,
/// 4-bit indices, the first index's top bit implied zero.
std::array<uint8_t, 64> decodeMode6(const std::array<uint8_t, 16>& block) {
    int at = 0;
    REQUIRE(bits(block, at, 7) == (1u << 6));
    uint32_t e[2][4];
    for (int c = 0; c < 4; ++c) {
        e[0][c] = bits(block, at, 7);
        e[1][c] = bits(block, at, 7);
    }
    const uint32_t p0 = bits(block, at, 1), p1 = bits(block, at, 1);
    for (int c = 0; c < 4; ++c) {
        e[0][c] = (e[0][c] << 1) | p0;
        e[1][c] = (e[1][c] << 1) | p1;
    }
    static constexpr uint32_t kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    std::array<uint8_t, 64> out{};
    for (int t = 0; t < 16; ++t) {
        const uint32_t index = bits(block, at, t == 0 ? 3 : 4);
        for (int c = 0; c < 4; ++c) {
            out[t * 4 + c] = static_cast<uint8_t>(((64 - kWeights[index]) * e[0][c] + kWeights[index] * e[1][c] + 32) >> 6);
        }
    }
    return out;
}

int worstError(const uint8_t (&texels)[64], const std::array<uint8_t, 64>& decoded) {
    int worst = 0;
    for (int i = 0; i < 64; ++i) worst = std::max(worst, std::abs(int(texels[i]) - int(decoded[i])));
    return worst;
}

} // namespace

TEST_CASE("a BC7 block reads back close to the normals and heights it was made from", "[normal_maps]") {
    SECTION("a flat patch reads back flat") {
        uint8_t texels[64];
        for (int t = 0; t < 16; ++t) {
            texels[t * 4 + 0] = 128;
            texels[t * 4 + 1] = 128;
            texels[t * 4 + 2] = 255;
            texels[t * 4 + 3] = 128;
        }
        CHECK(worstError(texels, decodeMode6(encodeBc7Block(texels))) <= 1);
    }

    SECTION("a slope - x rising, y falling, the height rising with x") {
        uint8_t texels[64];
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const int t = y * 4 + x;
                texels[t * 4 + 0] = static_cast<uint8_t>(100 + x * 12);
                texels[t * 4 + 1] = static_cast<uint8_t>(160 - x * 9);
                texels[t * 4 + 2] = 240;
                texels[t * 4 + 3] = static_cast<uint8_t>(60 + x * 30);
            }
        }
        CHECK(worstError(texels, decodeMode6(encodeBc7Block(texels))) <= 6);
    }

    SECTION("the first texel at the far end of the range") {
        uint8_t texels[64];
        for (int t = 0; t < 16; ++t) {
            const uint8_t v = static_cast<uint8_t>(t == 0 ? 250 : 10 + t * 8);
            for (int c = 0; c < 4; ++c) texels[t * 4 + c] = v;
        }
        CHECK(worstError(texels, decodeMode6(encodeBc7Block(texels))) <= 10);
    }
}

TEST_CASE("a normal map's mip chain is encoded down to one texel, at a quarter of RGBA8", "[normal_maps]") {
    constexpr uint32_t kSize = 64;
    std::vector<uint8_t> rgba(kSize * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            uint8_t* p = &rgba[(y * kSize + x) * 4];
            p[0] = static_cast<uint8_t>(128 + 60 * std::sin(x * 0.2));
            p[1] = static_cast<uint8_t>(128 + 60 * std::cos(y * 0.15));
            p[2] = 230;
            p[3] = static_cast<uint8_t>((x + y) * 2);
        }
    }
    const EncodedNormalMap rgbaMap = encodeNormalMap(rgba, kSize, kSize, NormalMapFormat::Rgba8, 0.3f);
    const EncodedNormalMap bc7Map = encodeNormalMap(rgba, kSize, kSize, NormalMapFormat::Bc7, 0.3f);

    REQUIRE(bc7Map.valid());
    CHECK(bc7Map.levels.size() == 7);  // 64, 32, 16, 8, 4, 2, 1
    CHECK(rgbaMap.levels.size() == 7);
    CHECK(bc7Map.heightVariance == 0.3f);
    // The largest levels are a quarter the size; the 2x2 and 1x1 levels are a
    // whole block each.
    CHECK(bc7Map.levels[0].size() * 4 == rgbaMap.levels[0].size());
    CHECK(bc7Map.levels[6].size() == 16);
    CHECK(bc7Map.bytes() * 3 < rgbaMap.bytes());

    // The top level's first block reads back close to its texels.
    uint8_t texels[64];
    for (int ty = 0; ty < 4; ++ty) {
        std::copy_n(&rgba[(ty * kSize) * 4], 16, &texels[ty * 16]);
    }
    std::array<uint8_t, 16> block{};
    std::copy_n(bc7Map.levels[0].begin(), 16, block.begin());
    CHECK(worstError(texels, decodeMode6(block)) <= 8);
}
