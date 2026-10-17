#include "rendering/wind_waves.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace wowee {
namespace rendering {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kMetresPerYard = 0.9144;
constexpr double kGravity = 9.80665 / kMetresPerYard;                          // yards / s^2
constexpr double kTensionOverDensity = 7.28e-5 / (kMetresPerYard * kMetresPerYard * kMetresPerYard);  // yd^3 / s^2

constexpr double kShortestWavelength = 0.3;  // yards: below this a pixel never shows one
constexpr double kLongestWavelength = 30.0;  // yards: the swell on the mesh takes over
constexpr double kGoldenAngle = 2.39996322972865332;  // radians

constexpr int kDrawnPerBand = kWindWaveCount / kWindWaveBands;
static_assert(kDrawnPerBand * kWindWaveBands == kWindWaveCount, "every band draws alike");

/// Donelan and Banner's spreading: how narrowly waves of angular frequency
/// `omega` keep to the wind, for a sea whose peak is at `peakOmega`, as the
/// beta of a sech^2(beta theta) spread. Narrowest just above the peak
/// (beta 2.3, about twenty degrees either way), and widening toward the
/// short chop to beta 0.4 - wide, but still several times stronger downwind
/// than against it. Mitsuyasu's cos^2s, whose s falls toward nothing for
/// short waves, ran the chop as hard into the wind as with it.
double spreadingBeta(double omega, double peakOmega) {
    const double r = std::max(omega / peakOmega, 0.56);
    if (r < 0.95) return 2.61 * std::pow(r, 1.3);
    if (r < 1.6) return 2.28 * std::pow(r, -1.3);
    return std::pow(10.0, -0.4 + 0.8393 * std::exp(-0.567 * std::log(r * r)));
}

const std::array<WindWave, kWindWaveTrains>& allTrains() {
    static const std::array<WindWave, kWindWaveTrains> trains = [] {
        std::array<WindWave, kWindWaveTrains> t{};
        for (int i = 0; i < kWindWaveTrains; ++i) t[i] = windWaveTrain(i);
        return t;
    }();
    return trains;
}

/// The mean square slope the wind asks of each train once the sea has
/// caught up with it. Each band takes its share - equal for every band in
/// the saturated range, falling away below the Phillips peak - and splits it
/// among four of its ways picked at the quarters of the band's spreading
/// (each the way the distribution's 1/8, 3/8, 5/8 and 7/8 fall in), a
/// quarter to each. Four ways stand for the whole spread that way: close to
/// the wind where it is narrow, wide where it is wide. Taking the four
/// steepest instead took the four closest to the wind for every band.
std::array<double, kWindWaveTrains> targetSlopeSq(float speedYardsPerSecond, glm::vec2 windDirection) {
    std::array<double, kWindWaveTrains> target{};
    const double meanSquareSlope = std::pow(static_cast<double>(windWaveSlope(speedYardsPerSecond)), 2.0);
    const double speed = std::max(static_cast<double>(speedYardsPerSecond), 0.0);
    if (meanSquareSlope <= 0.0 || speed <= 0.0) return target;  // still air: a mirror
    const float wl = glm::length(windDirection);
    const glm::vec2 toward = wl > 1e-6f ? windDirection / wl : glm::vec2(1.0f, 0.0f);
    const double peakWavenumber = kGravity / (speed * speed);  // Phillips
    const double peakOmega = std::sqrt(kGravity * peakWavenumber);
    const auto& trains = allTrains();

    std::array<double, kWindWaveBands> bandShare{};
    double totalShare = 0.0;
    for (int band = 0; band < kWindWaveBands; ++band) {
        const double below = peakWavenumber / trains[band * kWindWaveDirections].wavenumber;
        bandShare[band] = std::exp(-below * below);
        totalShare += bandShare[band];
    }
    if (totalShare <= 0.0) return target;

    for (int band = 0; band < kWindWaveBands; ++band) {
        const int first = band * kWindWaveDirections;
        const double beta = spreadingBeta(trains[first].angularSpeed, peakOmega);
        std::array<double, kWindWaveDirections> spread{};
        double sum = 0.0;
        for (int way = 0; way < kWindWaveDirections; ++way) {
            const double cosine = std::clamp(static_cast<double>(glm::dot(trains[first + way].direction, toward)),
                                             -1.0, 1.0);
            const double sech = 1.0 / std::cosh(beta * std::acos(cosine));
            spread[way] = sech * sech;
            sum += spread[way];
        }
        if (sum <= 0.0) continue;
        // Walk the ways from the one running against the wind round to it
        // again, so the quarters fall in order of angle from the wind.
        int start = 0;
        for (int way = 1; way < kWindWaveDirections; ++way) {
            if (spread[way] < spread[start]) start = way;
        }
        const double quarter = meanSquareSlope * bandShare[band] / totalShare / kDrawnPerBand;
        double cumulative = 0.0;
        int pick = 0;
        for (int step = 0; step < kWindWaveDirections && pick < kDrawnPerBand; ++step) {
            const int way = (start + step) % kWindWaveDirections;
            cumulative += spread[way] / sum;
            while (pick < kDrawnPerBand &&
                   cumulative >= (pick + 0.5) / kDrawnPerBand - 1e-12) {
                target[first + way] += quarter;
                ++pick;
            }
        }
    }
    return target;
}

/// The drawn set out of every train's mean square slope: per band, its
/// kDrawnPerBand steepest, with the amplitudes those slopes give.
std::array<WindWave, kWindWaveCount> drawnFrom(const std::array<double, kWindWaveTrains>& slopeSq) {
    std::array<WindWave, kWindWaveCount> drawn{};
    const auto& trains = allTrains();
    int next = 0;
    for (int band = 0; band < kWindWaveBands; ++band) {
        std::array<int, kWindWaveDirections> order{};
        std::iota(order.begin(), order.end(), band * kWindWaveDirections);
        std::partial_sort(order.begin(), order.begin() + kDrawnPerBand, order.end(),
                          [&](int a, int b) { return slopeSq[a] > slopeSq[b]; });
        for (int i = 0; i < kDrawnPerBand; ++i) {
            WindWave w = trains[order[i]];
            // A sine of amplitude a and wavenumber k has a mean square slope
            // of (k a)^2 / 2.
            w.amplitude = static_cast<float>(std::sqrt(2.0 * slopeSq[order[i]]) / w.wavenumber);
            drawn[next++] = w;
        }
    }
    return drawn;
}

} // namespace

float windWaveSlope(float speedYardsPerSecond) {
    const double metresPerSecond = std::max(static_cast<double>(speedYardsPerSecond), 0.0) * kMetresPerYard;
    return static_cast<float>(std::sqrt(0.00512 * metresPerSecond));  // the wind's part of Cox and Munk, 1954
}

float waterWaveAngularSpeed(float wavenumber) {
    const double k = std::max(static_cast<double>(wavenumber), 0.0);
    return static_cast<float>(std::sqrt(kGravity * k + kTensionOverDensity * k * k * k));
}

WindWave windWaveTrain(int train) {
    const int band = train / kWindWaveDirections;
    const int way = train % kWindWaveDirections;
    const double t = static_cast<double>(band) / (kWindWaveBands - 1);
    const double wavelength = kLongestWavelength * std::pow(kShortestWavelength / kLongestWavelength, t);
    // The band's compass, turned from the last band's by the golden angle so
    // no two bands line their trains up.
    const double angle = kGoldenAngle * band + kTwoPi * way / kWindWaveDirections;
    WindWave w;
    w.train = train;
    w.direction = glm::vec2(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
    w.wavenumber = static_cast<float>(kTwoPi / wavelength);
    w.angularSpeed = waterWaveAngularSpeed(w.wavenumber);
    return w;
}

std::array<WindWave, kWindWaveCount> windWavesFor(float speedYardsPerSecond, glm::vec2 windDirection) {
    return drawnFrom(targetSlopeSq(speedYardsPerSecond, windDirection));
}

float windWaveRide(const std::array<WindWave, kWindWaveCount>& waves) {
    // Points fold over where the stretch of the surface, 1 - sum of ride k a
    // sin, reaches zero; at worst every sine is 1 together.
    constexpr double kMostSteepness = 0.9;
    double steepness = 0.0;
    for (const WindWave& w : waves) steepness += static_cast<double>(w.wavenumber) * w.amplitude;
    if (steepness <= kMostSteepness) return 1.0f;
    return static_cast<float>(kMostSteepness / steepness);
}

void WindSea::advance(double deltaSeconds, float speedYardsPerSecond, glm::vec2 windDirection) {
    const auto target = targetSlopeSq(speedYardsPerSecond, windDirection);
    const auto& trains = allTrains();
    for (int i = 0; i < kWindWaveTrains; ++i) {
        // Running phase, carried forward rather than taken as a product with
        // the clock, which would lose its fraction.
        phase_[i] = std::fmod(phase_[i] + trains[i].angularSpeed * deltaSeconds, kTwoPi);
        if (!started_) {
            slopeSq_[i] = target[i];
            continue;
        }
        // Each train grows toward what the wind asks of it, or dies away,
        // over kResponsePeriods of its own periods.
        const double period = kTwoPi / trains[i].angularSpeed;
        const double settle = 1.0 - std::exp(-deltaSeconds / (kResponsePeriods * period));
        slopeSq_[i] += (target[i] - slopeSq_[i]) * settle;
    }
    started_ = true;
}

std::array<WindWave, kWindWaveCount> WindSea::drawn() const {
    return drawnFrom(slopeSq_);
}

} // namespace rendering
} // namespace wowee
