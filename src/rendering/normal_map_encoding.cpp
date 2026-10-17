#include "rendering/normal_map_encoding.hpp"

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

namespace {

/// BC7's 4-bit interpolation weights, out of 64.
constexpr int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

/// Writes a 128-bit block from its least significant bit up.
class BlockWriter {
public:
    void put(uint32_t value, int bits) {
        for (int i = 0; i < bits; ++i, ++at_) {
            if ((value >> i) & 1u) bytes_[at_ / 8] |= static_cast<uint8_t>(1u << (at_ % 8));
        }
    }
    [[nodiscard]] const std::array<uint8_t, 16>& bytes() const { return bytes_; }

private:
    std::array<uint8_t, 16> bytes_{};
    int at_ = 0;
};

/// The 7-bit value and parity bit that decode closest to `endpoint`.
void quantizeEndpoint(const float (&endpoint)[4], uint8_t (&q)[4], uint8_t& parity) {
    float bestError = -1.0f;
    for (uint8_t p = 0; p < 2; ++p) {
        uint8_t candidate[4];
        float error = 0.0f;
        for (int c = 0; c < 4; ++c) {
            const float v = std::round((endpoint[c] - static_cast<float>(p)) / 2.0f);
            candidate[c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 127.0f));
            const float decoded = static_cast<float>((candidate[c] << 1) | p);
            error += (decoded - endpoint[c]) * (decoded - endpoint[c]);
        }
        if (bestError < 0.0f || error < bestError) {
            bestError = error;
            parity = p;
            std::copy(candidate, candidate + 4, q);
        }
    }
}

std::vector<uint8_t> halve(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                           uint32_t& outWidth, uint32_t& outHeight) {
    outWidth = std::max(1u, width / 2);
    outHeight = std::max(1u, height / 2);
    std::vector<uint8_t> out(static_cast<size_t>(outWidth) * outHeight * 4);
    for (uint32_t y = 0; y < outHeight; ++y) {
        for (uint32_t x = 0; x < outWidth; ++x) {
            const uint32_t x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
            const uint32_t y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](uint32_t px, uint32_t py) {
                    return static_cast<uint32_t>(rgba[(static_cast<size_t>(py) * width + px) * 4 + c]);
                };
                out[(static_cast<size_t>(y) * outWidth + x) * 4 + c] =
                    static_cast<uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
            }
        }
    }
    return out;
}

std::vector<uint8_t> encodeBc7Level(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    const uint32_t blocksX = (width + 3) / 4;
    const uint32_t blocksY = (height + 3) / 4;
    std::vector<uint8_t> out(static_cast<size_t>(blocksX) * blocksY * 16);
    uint8_t texels[64];
    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            // A block past the edge of a small level repeats the edge texels.
            for (uint32_t ty = 0; ty < 4; ++ty) {
                for (uint32_t tx = 0; tx < 4; ++tx) {
                    const uint32_t x = std::min(bx * 4 + tx, width - 1);
                    const uint32_t y = std::min(by * 4 + ty, height - 1);
                    std::copy_n(&rgba[(static_cast<size_t>(y) * width + x) * 4], 4, &texels[(ty * 4 + tx) * 4]);
                }
            }
            const auto block = encodeBc7Block(texels);
            std::copy(block.begin(), block.end(), &out[(static_cast<size_t>(by) * blocksX + bx) * 16]);
        }
    }
    return out;
}

} // namespace

size_t EncodedNormalMap::bytes() const {
    size_t total = 0;
    for (const auto& level : levels) total += level.size();
    return total;
}

std::array<uint8_t, 16> encodeBc7Block(const uint8_t (&texels)[64]) {
    // Endpoints: the corners of the box the texels fill, inset by a sixteenth
    // of its size, along the diagonal the colours actually run - a channel
    // that falls as the widest one rises has its corners swapped.
    float lo[4], hi[4];
    for (int c = 0; c < 4; ++c) {
        lo[c] = 255.0f;
        hi[c] = 0.0f;
        for (int t = 0; t < 16; ++t) {
            lo[c] = std::min(lo[c], static_cast<float>(texels[t * 4 + c]));
            hi[c] = std::max(hi[c], static_cast<float>(texels[t * 4 + c]));
        }
    }
    int widest = 0;
    for (int c = 1; c < 4; ++c) {
        if (hi[c] - lo[c] > hi[widest] - lo[widest]) widest = c;
    }
    float mean[4] = {};
    for (int c = 0; c < 4; ++c) {
        for (int t = 0; t < 16; ++t) mean[c] += static_cast<float>(texels[t * 4 + c]) / 16.0f;
    }
    for (int c = 0; c < 4; ++c) {
        if (c == widest) continue;
        float covariance = 0.0f;
        for (int t = 0; t < 16; ++t) {
            covariance += (static_cast<float>(texels[t * 4 + widest]) - mean[widest]) *
                          (static_cast<float>(texels[t * 4 + c]) - mean[c]);
        }
        if (covariance < 0.0f) std::swap(lo[c], hi[c]);
    }
    for (int c = 0; c < 4; ++c) {
        const float inset = (hi[c] - lo[c]) / 32.0f;
        lo[c] += inset;
        hi[c] -= inset;
    }

    uint8_t q[2][4];
    uint8_t parity[2];
    quantizeEndpoint(lo, q[0], parity[0]);
    quantizeEndpoint(hi, q[1], parity[1]);

    // The palette the decoder will make from what was quantized, and each
    // texel's nearest entry in it.
    int palette[16][4];
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            const int e0 = (q[0][c] << 1) | parity[0];
            const int e1 = (q[1][c] << 1) | parity[1];
            palette[i][c] = ((64 - kWeights4[i]) * e0 + kWeights4[i] * e1 + 32) >> 6;
        }
    }
    uint8_t index[16];
    for (int t = 0; t < 16; ++t) {
        int bestError = -1;
        for (int i = 0; i < 16; ++i) {
            int error = 0;
            for (int c = 0; c < 4; ++c) {
                const int d = palette[i][c] - texels[t * 4 + c];
                error += d * d;
            }
            if (bestError < 0 || error < bestError) {
                bestError = error;
                index[t] = static_cast<uint8_t>(i);
            }
        }
    }

    // The first texel's index is stored without its top bit, which must be
    // zero: where it is not, the endpoints swap and the indices turn round.
    if (index[0] >= 8) {
        std::swap(q[0], q[1]);
        std::swap(parity[0], parity[1]);
        for (auto& i : index) i = static_cast<uint8_t>(15 - i);
    }

    BlockWriter block;
    block.put(1u << 6, 7);  // mode 6
    for (int c = 0; c < 4; ++c) {
        block.put(q[0][c], 7);
        block.put(q[1][c], 7);
    }
    block.put(parity[0], 1);
    block.put(parity[1], 1);
    block.put(index[0], 3);
    for (int t = 1; t < 16; ++t) block.put(index[t], 4);
    return block.bytes();
}

EncodedNormalMap encodeNormalMap(std::vector<uint8_t> rgba, uint32_t width, uint32_t height,
                                 NormalMapFormat format, float heightVariance) {
    EncodedNormalMap out;
    if (width == 0 || height == 0 || rgba.size() < static_cast<size_t>(width) * height * 4) return out;
    out.format = format;
    out.width = width;
    out.height = height;
    out.heightVariance = heightVariance;
    uint32_t w = width, h = height;
    for (;;) {
        out.levels.push_back(format == NormalMapFormat::Bc7 ? encodeBc7Level(rgba, w, h) : rgba);
        if (w == 1 && h == 1) break;
        uint32_t nextW = 0, nextH = 0;
        rgba = halve(rgba, w, h, nextW, nextH);
        w = nextW;
        h = nextH;
    }
    return out;
}

} // namespace rendering
} // namespace wowee
