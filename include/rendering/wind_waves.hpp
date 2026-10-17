#pragma once

#include <glm/vec2.hpp>

#include <array>

namespace wowee {
namespace rendering {

/// The wind's waves are made of trains: eight bands of wavelength from a
/// hand's breadth to thirty yards, each with sixteen ways round the compass.
/// A sea's crests are as short as the spread of ways its waves run in: one
/// way to a wavelength drew every crest as a straight line across the view.
/// Sixteen, because the long waves keep within twenty degrees of the wind,
/// and with eight the nearest way could be twenty-two and a half off.
inline constexpr int kWindWaveBands = 8;
inline constexpr int kWindWaveDirections = 16;
inline constexpr int kWindWaveTrains = kWindWaveBands * kWindWaveDirections;
/// How many of them the water is drawn with: the steepest in this wind.
inline constexpr int kWindWaveCount = 32;

/// One train of the waves the wind raises on the water: sine crests running
/// one way, at one wavelength.
struct WindWave {
    int train = 0;                    ///< which of the kWindWaveTrains it is
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

/// Train `train` (0 to kWindWaveTrains - 1) as it always is: its wavelength,
/// the way it runs and its speed, with no amplitude.
[[nodiscard]] WindWave windWaveTrain(int train);

/// The wind's waves under a wind of `speedYardsPerSecond` (speed times gust)
/// blowing toward `windDirection`: the kWindWaveCount steepest trains. The
/// trains themselves never change: their wavelengths and the ways they run
/// are fixed in the world, so a wind that veers does not turn crests that
/// stretch for thousands of yards (their phase there would race). What the
/// wind sets is how big each one is:
///  - together they are as steep as windWaveSlope says;
///  - each band takes an equal share of that steepness, as the saturated
///    part of a wind sea does (its height spectrum falls as k^-3), and bands
///    of waves longer than the wind can raise fall away below the Phillips
///    peak, at a wavenumber of g / U^2;
///  - a band's share goes to its ways by Donelan and Banner's spreading,
///    sech^2 of beta times the angle from the wind: beta is 2.3 near the
///    peak, so the long waves keep within twenty degrees of the wind in long
///    crests, and falls to 0.4 for the short chop, which crosses itself in
///    short ones but still runs mostly downwind. Four ways a band stand for
///    the whole spread, taken at its quarters.
/// This is the sea once it has caught up with the wind; WindSea is the sea
/// on its way there.
[[nodiscard]] std::array<WindWave, kWindWaveCount> windWavesFor(float speedYardsPerSecond,
                                                                glm::vec2 windDirection);

/// The wind's sea as it stands: every train's steepness and running phase.
/// A sea does not follow its wind at once: each train grows toward what the
/// wind asks of it, or dies away, over kResponsePeriods of its own periods -
/// the short chop in seconds, thirty-yard waves over a minute and more - so
/// a wind that rises, drops or turns changes the water as it would, rather
/// than in a jump. The first advance is the sea as the wind has it.
class WindSea {
public:
    static constexpr double kResponsePeriods = 20.0;

    void advance(double deltaSeconds, float speedYardsPerSecond, glm::vec2 windDirection);
    /// The kWindWaveCount trains to draw: per band, its steepest now.
    [[nodiscard]] std::array<WindWave, kWindWaveCount> drawn() const;
    /// Train `train`'s phase now, radians in [0, 2 pi).
    [[nodiscard]] double phase(int train) const { return phase_[train]; }

private:
    std::array<double, kWindWaveTrains> slopeSq_{};
    std::array<double, kWindWaveTrains> phase_{};
    bool started_ = false;
};

/// How far a point of the water rides along with the waves, 0 to 1. Under a
/// passing wave a point of water goes round a circle as wide as the wave is
/// high, which carries it toward each crest and away from each trough:
/// crests sharp, troughs wide. Where the trains together are steep enough
/// that their circles would cross - the sum of k a over 0.9 - the surface
/// would fold over itself, and the ride is cut to keep it from folding.
[[nodiscard]] float windWaveRide(const std::array<WindWave, kWindWaveCount>& waves);

} // namespace rendering
} // namespace wowee
