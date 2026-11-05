#pragma once

/// Where the sun is: the direction its light comes from over the day, as the
/// client's day tables give it, and where it falls on the screen.

#include <cmath>
#include <span>

#include <glm/glm.hpp>

#include "pipeline/coordinates.hpp"

namespace wowee::rendering {

/// One key of a table over the day: where in the day it sits, as a fraction
/// of the day from midnight to midnight, and the value there.
struct DayKey {
    float dayProgression;
    float value;
};

/// A table's value at a moment of the day, as the client's `InterpTable`
/// reads it: linear between the key at or after the moment and the key before
/// it, and across midnight between the last key and the first.
inline float interpolateDayTable(std::span<const DayKey> table, float dayProgression) {
    const size_t count = table.size();
    size_t after = 0;
    while (after < count && dayProgression > table[after].dayProgression) ++after;
    if (after == count) after = 0;
    const size_t before = (after == 0) ? count - 1 : after - 1;

    float span = table[after].dayProgression - table[before].dayProgression;
    float elapsed = dayProgression - table[before].dayProgression;
    if (span < 0.0f) span += 1.0f;
    if (elapsed < 0.0f) elapsed += 1.0f;
    const float t = (span > 0.0f) ? elapsed / span : 0.0f;
    return table[before].value + t * (table[after].value - table[before].value);
}

/// The direction the sun's light travels at a moment of the day, in render
/// axes, as WoW.exe sets it (DayNight::SetDirection).
///
/// The client's sun never sets. Its light always comes from the same quarter
/// of the sky, at an elevation that breathes between 37 degrees at midnight
/// and noon and 20 degrees at six in the morning and evening; what makes the
/// evening is the colour and strength of the light bands, not the angle. Flat
/// ground therefore takes direct light at every hour, at a third of the
/// band's strength at the lowest.
///
/// The sun was being walked across the sky instead, from a sunrise at six to
/// a sunset at six, so from then until midnight the light came from under the
/// ground and flat ground got the ambient band alone: a dusk sky from the
/// tables over a ground as dark as night, while the client at the same hour
/// still showed its streets lit.
inline glm::vec3 sunLightDirection(float dayProgression) {
    // The polar angle from straight up and the azimuth, in the client's
    // world axes, keyed on the day.
    static constexpr DayKey kPolar[] = {
        {0.00f, 2.2165682f}, {0.25f, 1.9198622f}, {0.50f, 2.2165682f}, {0.75f, 1.9198622f}};
    static constexpr DayKey kAzimuth[] = {
        {0.00f, 3.9269907f}, {0.25f, 3.9269907f}, {0.50f, 3.9269907f}, {0.75f, 3.9269907f}};

    dayProgression -= std::floor(dayProgression);
    const float polar = interpolateDayTable(kPolar, dayProgression);
    const float azimuth = interpolateDayTable(kAzimuth, dayProgression);
    const glm::vec3 canonical(std::sin(polar) * std::cos(azimuth),
                              std::sin(polar) * std::sin(azimuth),
                              std::cos(polar));
    return glm::normalize(core::coords::canonicalToRender(canonical));
}

/// Where the sun falls on the screen.
struct SunOnScreen {
    /// False when the sun is behind the eye, where it has no place on screen.
    bool inFront = false;
    /// Framebuffer uv, (0,0) at the top left - the way every full-screen pass
    /// samples. Not clamped: a sun just past the edge still streams rays in.
    glm::vec2 uv{0.0f};
};

/// The sun's place on screen, for the view and projection the frame was drawn
/// with. A direction, so it is projected with w = 0: the sun is infinitely far,
/// and moving the camera does not move it.
inline SunOnScreen sunScreenPosition(const glm::mat4& view, const glm::mat4& projection,
                                     const glm::vec3& sunDir) {
    const glm::vec4 clip = projection * view * glm::vec4(sunDir, 0.0f);
    if (clip.w <= 1e-4f) return {};
    return {.inFront = true, .uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f};
}

}  // namespace wowee::rendering
