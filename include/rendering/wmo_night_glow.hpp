#pragma once

/// How strongly a building's night-lit materials glow at a moment of the day.
///
/// A WMO material with F_SIDN (MOMT flag 0x10) - the stained glass of
/// Dalaran, the windows of Stormwind and Goldshire, lamp glass - carries a
/// colour it glows in after dark, its sidnColor. WoW.exe 12340 scales that
/// colour every frame by one factor over the day (CMapObj update 0x7a8520,
/// the factor kept at 0xd38cdc and set from the table at 0xaf4c80 in the light
/// update 0x7f38cc): nothing from 07:00 to 20:30, rising to full by 21:30,
/// full through the night, and gone again over 06:00 to 07:00. The scaled
/// colour is added to the light of the batch before the texture multiplies
/// it, and only in a batch lit by the sun: an F_UNLIT material and an interior
/// batch lit by its vertex colours alone never glow.
///
/// By day the material is the plain textured surface its shader says it is.
/// Glass is not drawn as anything else - no reflection, no Fresnel term, no
/// transparency - in shader 0 or 1, which nearly every window uses.

#include <cmath>

#include "rendering/sun_direction.hpp"

namespace wowee::rendering {

/// The glow factor at a moment of the day, as a fraction of the day from
/// midnight: 0 by day, about 1 at night.
inline float wmoNightGlow(float dayProgression) {
    static constexpr DayKey kGlow[] = {
        {0.25f, 1.0f}, {0.2916667f, 0.0f}, {0.8541666f, 0.0f}, {0.8958333f, 1.0f}};
    dayProgression -= std::floor(dayProgression);
    const float f = interpolateDayTable(kGlow, dayProgression);
    // The client keeps the colour in bytes: the factor goes through 255ths
    // (fistp of f * 255 - 0.5) and the product is shifted down by 8.
    return std::nearbyint(f * 255.0f - 0.5f) / 256.0f;
}

}  // namespace wowee::rendering
