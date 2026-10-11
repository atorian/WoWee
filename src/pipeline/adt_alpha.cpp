#include "pipeline/adt_alpha.hpp"

#include <algorithm>

namespace wowee {
namespace pipeline {

namespace {

/// The last row and column of a four-bit map are not data.
///
/// A chunk's alpha map is stored 64 wide and only 63 of those are painted: the
/// file's last row and column carry whatever was left there, and the client
/// fills them from the row and column before. Without it every chunk ends in a
/// strip of something else, and since the strip is on two of the four sides,
/// the boundary between two chunks is a hard line - which is the ground
/// textures not quite lining up, in a grid across the whole world.
///
/// Only the four-bit form. The eight-bit and compressed maps are what a map
/// with "big alpha" carries, and those are painted to the edge.
void fixLastRowAndColumn(std::vector<uint8_t>& alpha) {
    if (alpha.size() < ALPHA_MAP_SIZE) return;
    constexpr size_t kLast = ALPHA_MAP_DIM - 1;
    for (size_t i = 0; i < ALPHA_MAP_DIM; ++i) {
        alpha[kLast * ALPHA_MAP_DIM + i] = alpha[(kLast - 1) * ALPHA_MAP_DIM + i];
    }
    // After the row, so the corner takes the value the row above it just did.
    for (size_t i = 0; i < ALPHA_MAP_DIM; ++i) {
        alpha[i * ALPHA_MAP_DIM + kLast] = alpha[i * ALPHA_MAP_DIM + kLast - 1];
    }
}

}  // namespace

bool decodeLayerAlpha(const MapChunk& chunk, size_t layerIdx,
                      std::vector<uint8_t>& outAlpha, uint8_t unsetFill) {
    outAlpha.assign(ALPHA_MAP_SIZE, unsetFill);

    if (layerIdx >= chunk.layers.size()) return false;
    const auto& layer = chunk.layers[layerIdx];
    if (!layer.useAlpha() || layer.offsetMCAL >= chunk.alphaMap.size()) return false;

    const size_t offset = layer.offsetMCAL;

    // How much of MCAL belongs to this layer, taken from where the next layer
    // with an alpha map starts rather than from what is left in the blob. The
    // difference decides between the 4-bit and 8-bit forms below, so reading
    // "everything remaining" would misidentify every layer but the last.
    size_t layerEnd = chunk.alphaMap.size();
    for (size_t j = layerIdx + 1; j < chunk.layers.size(); ++j) {
        if (chunk.layers[j].useAlpha()) {
            layerEnd = std::min(layerEnd, static_cast<size_t>(chunk.layers[j].offsetMCAL));
            break;
        }
    }

    if (layerEnd < offset) return false;
    const size_t layerSize = layerEnd - offset;

    if (layer.compressedAlpha()) {
        size_t readPos = offset;
        size_t writePos = 0;
        while (writePos < ALPHA_MAP_SIZE && readPos < layerEnd) {
            const uint8_t cmd = chunk.alphaMap[readPos++];
            const bool fill = (cmd & ALPHA_FILL_FLAG) != 0;
            const int count = cmd & ALPHA_COUNT_MASK;
            // Zero-length commands consume only their command byte.
            if (count == 0) continue;

            if (fill) {
                if (readPos >= layerEnd) break;
                const uint8_t val = chunk.alphaMap[readPos++];
                for (int i = 0; i < count && writePos < ALPHA_MAP_SIZE; ++i) {
                    outAlpha[writePos++] = val;
                }
            } else {
                for (int i = 0;
                     i < count && writePos < ALPHA_MAP_SIZE && readPos < layerEnd;
                     ++i) {
                    outAlpha[writePos++] = chunk.alphaMap[readPos++];
                }
            }
        }
        return true;
    }

    if (layerSize >= ALPHA_MAP_SIZE) {
        std::copy(chunk.alphaMap.begin() + static_cast<std::ptrdiff_t>(offset),
                  chunk.alphaMap.begin() + static_cast<std::ptrdiff_t>(offset + ALPHA_MAP_SIZE),
                  outAlpha.begin());
        return true;
    }

    if (layerSize >= ALPHA_MAP_PACKED &&
        offset + ALPHA_MAP_PACKED <= chunk.alphaMap.size()) {
        // 4 bits per texel: low nibble first, scaled 0-15 to 0-255 by 17.
        for (size_t i = 0; i < ALPHA_MAP_PACKED; ++i) {
            const uint8_t v = chunk.alphaMap[offset + i];
            outAlpha[i * 2] = static_cast<uint8_t>((v & 0x0F) * 17);
            outAlpha[i * 2 + 1] = static_cast<uint8_t>((v >> 4) * 17);
        }
        if ((chunk.flags & 0x8000) == 0) fixLastRowAndColumn(outAlpha);
        return true;
    }

    return false;
}

// Render/query representation: absolute coverage, regardless of disk format.
bool decodeLayerWeight(const MapChunk& chunk, size_t layerIdx,
                       std::vector<uint8_t>& outWeight, uint8_t unsetFill) {
    if (!decodeLayerAlpha(chunk, layerIdx, outWeight, unsetFill)) return false;
    const auto& layer = chunk.layers[layerIdx];
    size_t end = chunk.alphaMap.size();
    for (size_t j = layerIdx + 1; j < chunk.layers.size(); ++j) {
        if (chunk.layers[j].useAlpha()) {
            end = std::min(end, static_cast<size_t>(chunk.layers[j].offsetMCAL));
            break;
        }
    }
    // Big alpha already stores coverage. Packed alpha stores opacity over
    // lower layers, so only that form needs the coverage of upper layers removed.
    if (layer.compressedAlpha() || end - layer.offsetMCAL >= ALPHA_MAP_SIZE) return true;
    std::vector<uint8_t> upper;
    for (size_t j = layerIdx + 1; j < std::min<size_t>(chunk.layers.size(), 4); ++j) {
        if (!decodeLayerAlpha(chunk, j, upper, unsetFill)) continue;
        for (size_t i = 0; i < ALPHA_MAP_SIZE; ++i) {
            outWeight[i] = static_cast<uint8_t>((outWeight[i] * (255 - upper[i]) + 127) / 255);
        }
    }
    return true;
}

size_t alphaTexelIndex(float u, float v) {
    const auto clamp01 = [](float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); };
    constexpr float kPainted = static_cast<float>(ALPHA_MAP_DIM - 1);
    const auto x = static_cast<size_t>(clamp01(u) * kPainted);
    const auto y = static_cast<size_t>(clamp01(v) * kPainted);
    return y * ALPHA_MAP_DIM + x;
}

float sampleAlpha(const std::vector<uint8_t>& alpha, float u, float v) {
    if (alpha.size() < ALPHA_MAP_SIZE) return 0.0f;
    const auto clamp01 = [](float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); };

    const float fx = clamp01(u) * static_cast<float>(ALPHA_MAP_DIM - 1);
    const float fy = clamp01(v) * static_cast<float>(ALPHA_MAP_DIM - 1);
    const auto x0 = static_cast<size_t>(fx);
    const auto y0 = static_cast<size_t>(fy);
    const size_t x1 = std::min(x0 + 1, ALPHA_MAP_DIM - 1);
    const size_t y1 = std::min(y0 + 1, ALPHA_MAP_DIM - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const float a00 = static_cast<float>(alpha[y0 * ALPHA_MAP_DIM + x0]);
    const float a10 = static_cast<float>(alpha[y0 * ALPHA_MAP_DIM + x1]);
    const float a01 = static_cast<float>(alpha[y1 * ALPHA_MAP_DIM + x0]);
    const float a11 = static_cast<float>(alpha[y1 * ALPHA_MAP_DIM + x1]);

    const float top = a00 + (a10 - a00) * tx;
    const float bottom = a01 + (a11 - a01) * tx;
    return (top + (bottom - top) * ty) / 255.0f;
}

} // namespace pipeline
} // namespace wowee
