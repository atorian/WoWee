#pragma once

/// Keeping a surface's total light inside what a colour can hold.
///
/// Every lit shader adds the two the same way: `ambient + diffuse * N·L`. On
/// ground facing the sun that is very nearly `ambient + diffuse`, and over the
/// 844 LightParams rows the client ships, 82% of them exceed 1.0 in some
/// channel at noon. The worst reaches 2.00.
///
/// What that costs is not brightness, it is colour. Clipping happens per
/// channel, so the channels that overflow stop at 1.0 while the others keep
/// their value, and the hue slides toward whichever channels saturated.
/// Mulgore's grass clips red and green and leaves blue behind, which is why it
/// reads as searing yellow rather than bright green, and Teldrassil's canopies
/// go magenta.
///
/// Scaling both terms by one factor keeps the hue and the ratio between
/// ambient and direct light, and only ever darkens: a zone already inside the
/// range is untouched.

#include <algorithm>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// The factor that brings `ambient + diffuse` inside 1.0, or 1 if it already is.
inline float lightHeadroomScale(const glm::vec3& ambient,
                                const glm::vec3& diffuse) {
    const glm::vec3 sum = ambient + diffuse;
    const float peak = std::max({sum.r, sum.g, sum.b});
    return (peak > 1.0f) ? (1.0f / peak) : 1.0f;
}

/// Scale both terms together so no channel of their sum passes 1.0.
inline void applyLightHeadroom(glm::vec3& ambient, glm::vec3& diffuse) {
    const float scale = lightHeadroomScale(ambient, diffuse);
    if (scale >= 1.0f) return;
    ambient *= scale;
    diffuse *= scale;
}

 
/// Transfer a small part of direct daylight into ambient fill without creating
/// energy or changing the colour of a sun-facing surface. In particular, the
/// negative-facing side of terrain/characters should not become nearly black
/// just because the authored DBC sun is strong.
/// 
/// Only soften broad daylight. The visual clock (rather than the server clock)
/// preserves fixed-night zones such as Duskwood and leaves night/torch contrast
/// alone. This is done before applyLightHeadroom so peak-channel clipping is
/// still prevented by its existing policy.
inline float outdoorDaylightWeight(float visualHour) {
    const auto smooth = [](float x, float from, float to) {
        const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    return smooth(visualHour, 6.0f, 9.0f) *
           (1.0f - smooth(visualHour, 17.0f, 20.0f));
}

inline void redistributeOutdoorDaylight(glm::vec3& ambient, glm::vec3& diffuse,
                                        float visualHour) {
    const float daylight = outdoorDaylightWeight(visualHour);
    if (daylight <= 0.0f) return;

    const float directPeak = std::max({diffuse.r, diffuse.g, diffuse.b});
    // Little/no direct light (dark or cloudy areas) already has soft shading.
    const float sunStrength = std::clamp((directPeak - 0.20f) / 0.50f, 0.0f, 1.0f);
    const float transfer = 0.20f * daylight * sunStrength;
    const glm::vec3 fill = diffuse * transfer;
    ambient += fill;
    diffuse -= fill;
}

}  // namespace wowee::rendering
