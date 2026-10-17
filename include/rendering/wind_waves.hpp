#pragma once

#include <glm/vec2.hpp>

#include <array>

namespace wowee {
namespace rendering {

/// How many trains of wind waves the water is drawn with: four to each
/// factor of 2.2 in wavelength, from a hand's breadth to thirty yards.
inline constexpr int kWindWaveCount = 24;

/// One train of the waves the wind raises on the water: sine crests running
/// one way, at one wavelength.
struct WindWave {
    glm::vec2 direction{1.0f, 0.0f};  ///< unit, render xy, the way the crests run
    float wavenumber = 0.0f;          ///< radians a yard: 2 pi over the wavelength
    float angularSpeed = 0.0f;        ///< radians a second, by water's dispersion
    float amplitude = 0.0f;           ///< yards, crest above the mean
};

/// How steep the chop the wind raises is: the root mean square slope (rise
/// over run). Cox and Munk measured the sea's from sun glitter as a mean
/// square slope of 0.003 + 0.00512 U for a wind of U m/s; the 0.003 is there
/// in still air, the swell's, which the waves on the mesh already draw, and
/// the rest is the wind's - 0.00512 U. Still air leaves the water a mirror
/// under its swell; a breeze of 5 m/s tilts it by nine degrees.
[[nodiscard]] float windWaveSlope(float speedYardsPerSecond);

/// How fast a wave of `wavenumber` (radians a yard) runs, in radians a
/// second: water's dispersion, gravity and surface tension both,
/// w^2 = g k + (tension / density) k^3. Long waves outrun short ones; only
/// at a few centimetres does surface tension take over.
[[nodiscard]] float waterWaveAngularSpeed(float wavenumber);

/// The wind's waves under a wind of `speedYardsPerSecond` (speed times gust)
/// blowing toward `windDirection`. The trains themselves never change: their
/// wavelengths and the ways they run are fixed in the world, spread round
/// the compass, so a wind that veers does not turn crests that stretch for
/// thousands of yards (their phase there would race). What the wind sets is
/// how big each one is:
///  - together they are as steep as windWaveSlope says;
///  - each wavelength takes an equal share of that steepness, as the
///    saturated part of a wind sea does (its height spectrum falls as k^-3);
///  - waves longer than the wind can raise fall away below the Phillips
///    peak, at a wavenumber of g / U^2;
///  - waves running downwind are largest, crosswind ones smaller and none
///    run into the wind (Longuet-Higgins' cos^4 of half the angle).
[[nodiscard]] std::array<WindWave, kWindWaveCount> windWavesFor(float speedYardsPerSecond,
                                                                glm::vec2 windDirection);

/// How far a point of the water rides along with the waves, 0 to 1. Under a
/// passing wave a point of water goes round a circle as wide as the wave is
/// high, which carries it toward each crest and away from each trough:
/// crests sharp, troughs wide. Where the trains together are steep enough
/// that their circles would cross - the sum of k a over 0.9 - the surface
/// would fold over itself, and the ride is cut to keep it from folding.
[[nodiscard]] float windWaveRide(const std::array<WindWave, kWindWaveCount>& waves);

} // namespace rendering
} // namespace wowee
