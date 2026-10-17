#pragma once

/// From the light volumes around the player to one set of LightingParams:
/// which LightParams row a volume names for the weather, what that row says at
/// an hour, and how the volumes add up.
///
/// Header-only, so the rules can be held against hand-built profiles without
/// the light tables loaded.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "rendering/light_coords.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/sun_direction.hpp"

namespace wowee::rendering {

/// Light.dbc and the band tables keep time of day in half-minutes, 0..2879.
inline constexpr uint16_t LIGHT_HALF_MINUTES_PER_DAY = 2880;

/// The LightParams row a volume names for the weather and the camera.
///
/// Light.dbc lists them as clear, clear underwater, storm, storm underwater.
/// A volume leaves a column at zero when it has nothing special for that case,
/// and then the next closest one stands in: under water the water rows win
/// over the storm, and a storm row stands in for no water row at all.
inline uint32_t selectLightParamsId(const LightVolume& volume, bool isRaining,
                                    bool isUnderwater) {
    if (isUnderwater) {
        if (isRaining && volume.lightParamsIdStormUnderwater != 0)
            return volume.lightParamsIdStormUnderwater;
        if (volume.lightParamsIdUnderwater != 0) return volume.lightParamsIdUnderwater;
    }
    if (isRaining && volume.lightParamsIdStorm != 0) return volume.lightParamsIdStorm;
    return volume.lightParamsId;
}

/// A band's value at a time of day, linear between its keyframes and wrapping
/// across midnight. `count` is at least one.
template <typename T>
T interpolateLightBand(const uint16_t* times, const T* values, uint8_t count,
                       uint16_t timeHalfMinutes) {
    if (count == 1) return values[0];

    uint8_t idx1 = count - 1;
    uint8_t idx2 = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (timeHalfMinutes < times[i]) {
            idx2 = i;
            idx1 = (i > 0) ? (i - 1) : (count - 1);
            break;
        }
    }

    const uint16_t t1 = times[idx1];
    const uint16_t t2 = times[idx2];
    const uint16_t timeSpan = (t2 > t1) ? (t2 - t1) : (LIGHT_HALF_MINUTES_PER_DAY - t1 + t2);
    const uint16_t elapsed = (timeHalfMinutes >= t1)
        ? (timeHalfMinutes - t1)
        : (LIGHT_HALF_MINUTES_PER_DAY - t1 + timeHalfMinutes);

    float t = (timeSpan > 0) ? (static_cast<float>(elapsed) / static_cast<float>(timeSpan)) : 0.0f;
    t = glm::clamp(t, 0.0f, 1.0f);
    return glm::mix(values[idx1], values[idx2], t);
}

inline glm::vec3 sampleColorBand(const ColorBand& band, uint16_t timeHalfMinutes) {
    if (band.numKeyframes == 0) return glm::vec3(0.5f);
    return interpolateLightBand(band.times, band.colors, band.numKeyframes, timeHalfMinutes);
}

/// Nothing when the band has no keyframes, so each caller picks the fallback
/// that means something for its channel. One shared 1.0 was a fog distance of
/// a thirty-sixth of a yard once converted, and drew everything fully fogged.
inline std::optional<float> sampleFloatBand(const FloatBand& band, uint16_t timeHalfMinutes) {
    if (band.numKeyframes == 0) return std::nullopt;
    return interpolateLightBand(band.times, band.values, band.numKeyframes, timeHalfMinutes);
}

/// What one LightParams row says at a time of day. A float channel the row
/// has no band for takes `fallback`'s value.
inline LightingParams sampleLightParams(const LightParamsProfile& profile,
                                        uint16_t timeHalfMinutes,
                                        const LightingParams& fallback) {
    using P = LightParamsProfile;
    LightingParams params = fallback;

    params.ambientColor = sampleColorBand(profile.colorBands[P::AMBIENT_COLOR], timeHalfMinutes);
    params.diffuseColor = sampleColorBand(profile.colorBands[P::DIFFUSE_COLOR], timeHalfMinutes);
    params.fogColor = sampleColorBand(profile.colorBands[P::FOG_COLOR], timeHalfMinutes);
    params.skyTopColor = sampleColorBand(profile.colorBands[P::SKY_TOP_COLOR], timeHalfMinutes);
    params.skyMiddleColor = sampleColorBand(profile.colorBands[P::SKY_MIDDLE_COLOR], timeHalfMinutes);
    params.skyBand1Color = sampleColorBand(profile.colorBands[P::SKY_BAND1_COLOR], timeHalfMinutes);
    params.skyBand2Color = sampleColorBand(profile.colorBands[P::SKY_BAND2_COLOR], timeHalfMinutes);
    params.sunColor = sampleColorBand(profile.colorBands[P::SUN_COLOR], timeHalfMinutes);
    params.sunHaloColor = sampleColorBand(profile.colorBands[P::SUN_HALO_COLOR], timeHalfMinutes);

    // The fog distance is stored in the same thirty-sixths of a yard as the
    // light positions, and was being used raw: Tirisfal's 12000 became 12000
    // yards, so the fog ended six times further out than the far clip and
    // nothing was ever hazed. It is 333 yards.
    if (const auto end = sampleFloatBand(profile.floatBands[P::FOG_END], timeHalfMinutes)) {
        params.fogEnd = *end / LIGHT_COORD_UNITS_PER_YARD;
    }
    // The start is a fraction of the end rather than a distance, so it needs
    // no conversion of its own.
    const auto startScalar =
        sampleFloatBand(profile.floatBands[P::FOG_START_SCALAR], timeHalfMinutes);
    const float fallbackScalar = fallback.fogEnd > 0.0f ? fallback.fogStart / fallback.fogEnd : 0.0f;
    params.fogStart = params.fogEnd * startScalar.value_or(fallbackScalar);

    if (const auto clouds = sampleFloatBand(profile.floatBands[P::CLOUD_DENSITY], timeHalfMinutes)) {
        params.cloudDensity = *clouds;
    }
    // No band holds a fog density, so fogDensity stays the fallback's.

    params.directionalDir =
        sunLightDirection(timeHalfMinutes / static_cast<float>(LIGHT_HALF_MINUTES_PER_DAY));
    return params;
}

/// A clear row's light blended toward its storm row by `w`
/// (sky_celestial::stormWeight of the weather), as WoW.exe blends
/// them (0x7ec220): every colour channel, the fog's distances and the cloud
/// cover, lerped. The light's direction, the fog density no band holds and
/// the horizon glow keep the clear row's.
inline LightingParams blendTowardStorm(const LightingParams& clear, const LightingParams& storm,
                                       float w) {
    LightingParams out = clear;
    out.ambientColor = glm::mix(clear.ambientColor, storm.ambientColor, w);
    out.diffuseColor = glm::mix(clear.diffuseColor, storm.diffuseColor, w);
    out.fogColor = glm::mix(clear.fogColor, storm.fogColor, w);
    out.skyTopColor = glm::mix(clear.skyTopColor, storm.skyTopColor, w);
    out.skyMiddleColor = glm::mix(clear.skyMiddleColor, storm.skyMiddleColor, w);
    out.skyBand1Color = glm::mix(clear.skyBand1Color, storm.skyBand1Color, w);
    out.skyBand2Color = glm::mix(clear.skyBand2Color, storm.skyBand2Color, w);
    out.sunColor = glm::mix(clear.sunColor, storm.sunColor, w);
    out.sunHaloColor = glm::mix(clear.sunHaloColor, storm.sunHaloColor, w);
    out.fogStart = glm::mix(clear.fogStart, storm.fogStart, w);
    out.fogEnd = glm::mix(clear.fogEnd, storm.fogEnd, w);
    out.cloudDensity = glm::mix(clear.cloudDensity, storm.cloudDensity, w);
    return out;
}

struct WeightedLightingParams {
    LightingParams params;
    float weight = 0.0f;
};

/// The weighted mean of the volumes that have a LightParams row, or nothing
/// when none of them carries weight.
///
/// Divided by the weight actually summed rather than trusting the volumes'
/// weights to add up to one: they were normalised before the rows were looked
/// up, so a volume naming a missing row took its share with it, and two
/// volumes at half each with one row missing drew the zone at half brightness
/// with its fog at half the distance.
inline std::optional<LightingParams> blendLightingParams(
        std::span<const WeightedLightingParams> parts) {
    LightingParams sum = LightingParams::blendAccumulator();
    glm::vec3 dir(0.0f);
    float total = 0.0f;
    for (const auto& [p, w] : parts) {
        if (w <= 0.0f) continue;
        sum.ambientColor += p.ambientColor * w;
        sum.diffuseColor += p.diffuseColor * w;
        sum.fogColor += p.fogColor * w;
        sum.skyTopColor += p.skyTopColor * w;
        sum.skyMiddleColor += p.skyMiddleColor * w;
        sum.skyBand1Color += p.skyBand1Color * w;
        sum.skyBand2Color += p.skyBand2Color * w;
        sum.sunColor += p.sunColor * w;
        sum.sunHaloColor += p.sunHaloColor * w;
        sum.fogStart += p.fogStart * w;
        sum.fogEnd += p.fogEnd * w;
        sum.fogDensity += p.fogDensity * w;
        sum.cloudDensity += p.cloudDensity * w;
        sum.horizonGlow += p.horizonGlow * w;
        dir += p.directionalDir * w;
        total += w;
    }
    if (total <= 0.0f) return std::nullopt;

    const float inv = 1.0f / total;
    sum.ambientColor *= inv;
    sum.diffuseColor *= inv;
    sum.fogColor *= inv;
    sum.skyTopColor *= inv;
    sum.skyMiddleColor *= inv;
    sum.skyBand1Color *= inv;
    sum.skyBand2Color *= inv;
    sum.sunColor *= inv;
    sum.sunHaloColor *= inv;
    sum.fogStart *= inv;
    sum.fogEnd *= inv;
    sum.fogDensity *= inv;
    sum.cloudDensity *= inv;
    sum.horizonGlow *= inv;

    const float dirLenSq = glm::dot(dir, dir);
    sum.directionalDir = dirLenSq > 1e-6f ? dir * glm::inversesqrt(dirLenSq)
                                          : glm::vec3(0.3f, -0.7f, 0.6f);  // all cancelled out
    return sum;
}

}  // namespace wowee::rendering
