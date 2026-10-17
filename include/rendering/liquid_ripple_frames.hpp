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

/// How much of its ripple a liquid shows under a wind of `speedYardsPerSecond`
/// (speed times gust), 0 to 1. The frames are a fine chop, and a fine chop is
/// what the wind raises: by the Beaufort scale still water is a mirror below
/// 0.5 m/s, scaled ripples start there, and by 3.4 m/s the wavelets are as
/// large as small waves get before crests break. So none below the first,
/// all of it from the second, and a smooth rise between.
[[nodiscard]] float rippleShareForWind(float speedYardsPerSecond);

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
