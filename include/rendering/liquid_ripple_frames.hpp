#pragma once

#include "pipeline/blp_loader.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee {
namespace rendering {

/// A liquid's animated surface texture, made ready to be one sampler2DArray:
/// the ripple of every frame as a single channel, with its mip chain.
///
/// LiquidType.dbc names the texture as a pattern over the frame number
/// ("XTextures\\ocean\\ocean_h.%d.blp", frames from 1). What the frames hold
/// differs: the lake and ocean sets are black with the ripple in the alpha,
/// the fast river set is opaque with it in the grey. Which one a set uses is
/// read off its first frame, not off its name.
struct LiquidRippleFrames {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frames = 0;
    /// levels[i]: mip i of every frame, one after another, a byte a texel.
    std::vector<std::vector<uint8_t>> levels;
    /// The ripple was in the alpha channel (true) or in the colour.
    bool fromAlpha = false;
};

/// How steep a liquid's ripple is under a wind of `speedYardsPerSecond`
/// (speed times gust): the root mean square slope (rise over run) of the
/// chop the wind raises. Cox and Munk measured the sea's from sun glitter as
/// a mean square slope of 0.003 + 0.00512 U for a wind of U m/s; the 0.003
/// is there in still air, the swell's, which the waves already draw, and the
/// rest is the wind's - so the ripple's is 0.00512 U. Still air leaves the
/// water a mirror under its swell; a breeze of 5 m/s tilts it by nine
/// degrees, not the forty-five the frames as painted gave.
[[nodiscard]] float rippleSlopeForWind(float speedYardsPerSecond);

/// The frames' own root mean square slope, in height (0..1) per repeat of
/// the texture, measured as the water shader takes it: central differences
/// one texel apart on the mip whose texel is `texels` texels of the full
/// frame. Dividing the shader's slope by this makes any liquid's frames as
/// steep as the wind says, whatever range their artist painted them in.
/// 0 for frames with no relief at all.
[[nodiscard]] float rippleSlopeRms(const LiquidRippleFrames& frames, uint32_t texels);

/// The path of frame `frame` (from 1) of a pattern; a pattern with no %d is
/// one frame, itself.
[[nodiscard]] std::string liquidFramePath(const std::string& pattern, uint32_t frame);

/// Load every frame of a pattern, from frame 1 until one is not there.
/// Frames of another size than the first end the set, said once. Nothing at
/// all - no first frame - is no set, and `whyNot` says why.
[[nodiscard]] std::optional<LiquidRippleFrames> loadLiquidRippleFrames(
    const std::string& pattern,
    const std::function<pipeline::BLPImage(const std::string&)>& loadFrame,
    std::string& whyNot);

} // namespace rendering
} // namespace wowee
