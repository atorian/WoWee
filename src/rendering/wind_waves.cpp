#include "rendering/wind_waves.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMetresPerYard = 0.9144;
constexpr double kGravity = 9.80665 / kMetresPerYard;                          // yards / s^2
constexpr double kTensionOverDensity = 7.28e-5 / (kMetresPerYard * kMetresPerYard * kMetresPerYard);  // yd^3 / s^2

constexpr double kShortestWavelength = 0.3;  // yards: below this a pixel never shows one
constexpr double kLongestWavelength = 30.0;  // yards: the swell on the mesh takes over
constexpr double kGoldenAngle = 2.39996322972865332;  // radians

} // namespace

float windWaveSlope(float speedYardsPerSecond) {
    const double metresPerSecond = std::max(static_cast<double>(speedYardsPerSecond), 0.0) * kMetresPerYard;
    return static_cast<float>(std::sqrt(0.00512 * metresPerSecond));  // the wind's part of Cox and Munk, 1954
}

float waterWaveAngularSpeed(float wavenumber) {
    const double k = std::max(static_cast<double>(wavenumber), 0.0);
    return static_cast<float>(std::sqrt(kGravity * k + kTensionOverDensity * k * k * k));
}

std::array<WindWave, kWindWaveCount> windWavesFor(float speedYardsPerSecond, glm::vec2 windDirection) {
    std::array<WindWave, kWindWaveCount> waves{};
    const double speed = std::max(static_cast<double>(speedYardsPerSecond), 0.0);
    const float wl = glm::length(windDirection);
    const glm::vec2 toward = wl > 1e-6f ? windDirection / wl : glm::vec2(1.0f, 0.0f);

    // The trains: log-spaced in wavelength, each its own way round the
    // compass by the golden angle, so no two neighbours run alike.
    std::array<double, kWindWaveCount> share{};
    double totalShare = 0.0;
    const double peakWavenumber = speed > 0.0 ? kGravity / (speed * speed) : 0.0;
    for (int i = 0; i < kWindWaveCount; ++i) {
        const double t = static_cast<double>(i) / (kWindWaveCount - 1);
        const double wavelength = kLongestWavelength * std::pow(kShortestWavelength / kLongestWavelength, t);
        const double angle = kGoldenAngle * i;
        WindWave& w = waves[i];
        w.direction = glm::vec2(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        w.wavenumber = static_cast<float>(2.0 * kPi / wavelength);
        w.angularSpeed = waterWaveAngularSpeed(w.wavenumber);

        const double below = peakWavenumber / w.wavenumber;              // Phillips
        const double cosine = glm::dot(w.direction, toward);
        const double spread = std::pow(0.5 * (1.0 + cosine), 2.0);       // cos^4 of half the angle
        share[i] = std::exp(-below * below) * spread;
        totalShare += share[i];
    }

    const double meanSquareSlope = std::pow(static_cast<double>(windWaveSlope(speedYardsPerSecond)), 2.0);
    if (meanSquareSlope <= 0.0 || totalShare <= 0.0) return waves;  // still air: a mirror
    for (int i = 0; i < kWindWaveCount; ++i) {
        // A sine of amplitude a and wavenumber k has a mean square slope of
        // (k a)^2 / 2; this train's share of the whole is share / total.
        const double slopeSq = meanSquareSlope * share[i] / totalShare;
        waves[i].amplitude = static_cast<float>(std::sqrt(2.0 * slopeSq) / waves[i].wavenumber);
    }
    return waves;
}

} // namespace rendering
} // namespace wowee
