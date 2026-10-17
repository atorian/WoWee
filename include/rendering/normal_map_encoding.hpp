#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

/// How a normal/height map is kept on the GPU.
enum class NormalMapFormat : uint8_t {
    /// Four bytes a texel: where the device cannot sample BC7.
    Rgba8,
    /// One byte a texel, all four channels: the normal in rgb, the height in a.
    Bc7,
};

/// A normal/height map ready to upload: every mip level, encoded, made on a
/// worker so the main thread only copies it to the GPU.
struct EncodedNormalMap {
    NormalMapFormat format = NormalMapFormat::Rgba8;
    /// Of level 0.
    uint32_t width = 0;
    uint32_t height = 0;
    /// Level 0 first, down to 1x1; BC7 levels are 4x4 blocks, row by row.
    std::vector<std::vector<uint8_t>> levels;
    /// The variance of the heights, which the parallax pass reads.
    float heightVariance = 0.0f;

    [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !levels.empty(); }
    [[nodiscard]] size_t bytes() const;
};

/// The mip chain of an RGBA8 normal/height map - each level the box filter
/// of the one above, as the GPU's blit made it before - in `format`.
[[nodiscard]] EncodedNormalMap encodeNormalMap(std::vector<uint8_t> rgba, uint32_t width, uint32_t height,
                                               NormalMapFormat format, float heightVariance);

/// One BC7 block, mode 6, of a 4x4 tile of RGBA8 texels given row by row.
[[nodiscard]] std::array<uint8_t, 16> encodeBc7Block(const uint8_t (&texels)[64]);

} // namespace rendering
} // namespace wowee
