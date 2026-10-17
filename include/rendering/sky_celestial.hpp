#pragma once

/// The sun as WoW.exe 3.3.5a (build 12340) shows it: where the disc stands at
/// an hour, how large it is, how it sinks under the horizon, and when and how
/// strongly its glare shows. Header-only, so the rules can be held to the
/// client's hours without a renderer.
///
/// The disc is not the directional light. The light comes from a table that
/// never lets the sun set (sun_direction.hpp); the disc follows a table of its
/// own and goes ten degrees under the horizon from 21:30 to 05:30. The client
/// has no time window for the disc - it is gone at night because it is below
/// the horizon - and its glare has keys of its own. This renderer had one
/// window for both, 05:00 to 19:00, under a sun the light still put 27 degrees
/// up, so an evening sky glowed round a sun that was not drawn.
///
/// Read from WoW.exe: the main sky update 0x7eecc0, disc path 0x7eefee-0x7ef183,
/// key lookup 0x7ed3b0, the disc quad and its horizon clip 0x7edee0, the glare
/// update 0x7ef6e0 and its draw 0x9ac400, the weather fade 0x7f3920/0x7f37c3,
/// and the light darkening when looking at the sun 0x781783.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include <glm/glm.hpp>

#include "core/coordinates.hpp"

namespace wowee::rendering::sky_celestial {

/// A value at a fraction of the day (0 = midnight).
struct DayKey {
    float day;
    float value;
};

/// The client's key lookup (0x7ed3b0): linear between keys, wrapping from the
/// last back to the first across midnight; a span under a thousandth of a day
/// holds the left key's value.
inline float lookup(std::span<const DayKey> keys, float day) {
    day = std::clamp(day, 0.0f, 1.0f);
    size_t left = keys.size() - 1;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].day <= day) left = i;
    }
    const size_t right = (left + 1) % keys.size();
    float span = keys[right].day - keys[left].day;
    if (span < 0.0f) span += 1.0f;
    float offset = day - keys[left].day;
    if (offset < 0.0f) offset += 1.0f;
    if (span < 0.001f) return keys[left].value;
    return keys[left].value + (keys[right].value - keys[left].value) * (offset / span);
}

/// The client's cosine of pi times x (used for every sky angle): a cubic per
/// half period, 1 - f^2 (6 - 4f), negated on odd periods. The floor is the
/// client's (0x5fe800): trunc above zero, trunc minus one at and below it.
inline float cosPi(float x) {
    const float n = x > 0.0f ? std::trunc(x) : std::trunc(x) - 1.0f;
    const float f = x - n;
    const float c = 1.0f - f * f * (6.0f - 4.0f * f);
    return (static_cast<int64_t>(n) & 1) ? -c : c;
}

/// The disc's angle from the zenith, radians: 100 degrees - under the
/// horizon - from 21:30 to 05:30, 5 degrees through midday.
inline constexpr DayKey kSunZenithAngle[] = {
    {0.229167f, 1.745329f}, {0.496528f, 0.087266f}, {0.5f, 0.087266f},
    {0.503472f, 0.087266f}, {0.895833f, 1.745329f}};
/// Its azimuth: 45 degrees, the same all day. It rises and sets on one side.
inline constexpr float kSunAzimuth = 0.785398f;
/// The disc's size, in units at the client's sky distance of twelve: one
/// through the day, two at sunrise and sunset.
inline constexpr DayKey kSunScale[] = {
    {0.25f, 2.0f}, {0.28125f, 1.0f}, {0.84375f, 1.0f}, {0.875f, 2.0f}};
/// The client draws sky objects twelve units from the eye.
inline constexpr float kSkyDistance = 12.0f;

/// The glare's own hours: in from 06:30 to 07:30, out from 19:30 to 21:00.
inline constexpr DayKey kSunGlareHours[] = {
    {0.270833f, 0.0f}, {0.3125f, 1.0f}, {0.8125f, 1.0f}, {0.875f, 0.0f}};

/// Where the disc stands and how it is cut by the horizon.
struct SunDisc {
    /// Toward the disc, unit, in render space.
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    /// The disc's centre height over the horizon at the sky distance, as the
    /// horizon clip measures it: 12 cos(zenith angle).
    float horizonHeight = 0.0f;
    /// Its width and height at the sky distance.
    float scale = 1.0f;
};

inline SunDisc sunDisc(float day) {
    const float theta = lookup(kSunZenithAngle, day);
    const float sinTheta = cosPi(theta * 0.31831f - 0.5f);
    const float cosTheta = cosPi(theta * 0.31831f);
    const float sinPhi = cosPi(kSunAzimuth * 0.31831f - 0.5f);
    const float cosPhi = cosPi(kSunAzimuth * 0.31831f);
    // The client's world axes: x north, y west, z up.
    const glm::vec3 canonical(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
    SunDisc disc;
    disc.direction = glm::normalize(core::coords::canonicalToRender(glm::normalize(canonical)));
    disc.horizonHeight = kSkyDistance * cosTheta;
    disc.scale = lookup(kSunScale, day);
    return disc;
}

/// How much of the disc shows at a point `local` of its height above its
/// centre (-0.5 bottom edge, 0.5 top edge): none under the horizon, fading in
/// over the 0.4 units above it (0x7edee0, alpha = clamp(2.5 h, 0, 1)).
inline float discHorizonAlpha(const SunDisc& disc, float local) {
    const float h = disc.horizonHeight + local * disc.scale;
    return std::clamp(2.5f * h, 0.0f, 1.0f);
}

/// Whether any of the disc is above the horizon.
inline bool discAboveHorizon(const SunDisc& disc) {
    return disc.horizonHeight + 0.5f * disc.scale > 0.0f;
}

/// What weather leaves of the disc and the glare: none once the weather is a
/// quarter of the way in (0x7f3920: w = min(1, 4 intensity), alpha 1 - w).
inline float weatherFade(float weatherIntensity) {
    return 1.0f - std::min(1.0f, 4.0f * std::max(weatherIntensity, 0.0f));
}

/// The glare as the camera sees it (0x7ef6e0), from `facing`, the cosine
/// between the view direction and the direction to the sun.
struct GlareShape {
    float size = 3.0f;   ///< width at the sky distance
    float alpha = 0.5f;  ///< before visibility
};
inline GlareShape glareShape(float facing) {
    const float f = (std::max(facing, 0.7f) - 0.7f) / 0.3f;
    return {.size = 3.0f + (20.0f - 3.0f) * f, .alpha = 0.5f + 0.5f * f};
}

/// The glare's visibility eased toward a target: up at 4 a second, down at
/// 1.51515 a second, from zero (0x7ef6e0).
inline float easeGlare(float current, float target, float dt) {
    if (target > current) return std::min(current + 4.0f * dt, target);
    return std::max(current - 1.51515f * dt, target);
}

/// How much the scene's direct and ambient light are dimmed when looking at
/// the sun (0x781783): 0.35 max(facing, 0.7)^10 of the glare's visibility, so
/// up to 35% staring straight at it.
inline float lookingAtSunDimming(float facing, float glareVisibility) {
    return 0.35f * std::pow(std::max(facing, 0.7f), 10.0f) * glareVisibility;
}

}  // namespace wowee::rendering::sky_celestial
