// The waves the wind raises on the water: how steep, how fast, which way and
// how long, as the sea has them.
#include <catch_amalgamated.hpp>

#include "rendering/wind_waves.hpp"

#include <glm/geometric.hpp>

#include <cmath>

using wowee::rendering::kWindWaveCount;
using wowee::rendering::waterWaveAngularSpeed;
using wowee::rendering::windWaveRide;
using wowee::rendering::windWavesFor;
using wowee::rendering::windWaveSlope;

namespace {

constexpr float kYardsPerMetre = 1.0936133f;
constexpr float kPi = 3.14159265f;

/// The mean square slope of a set of trains: (k a)^2 / 2 each.
template <typename Waves>
double meanSquareSlope(const Waves& waves) {
    double sum = 0.0;
    for (const auto& w : waves) sum += 0.5 * std::pow(static_cast<double>(w.wavenumber * w.amplitude), 2.0);
    return sum;
}

}  // namespace

TEST_CASE("still air raises no waves", "[water][wind]") {
    CHECK(windWaveSlope(0.0f) == 0.0f);
    for (const auto& w : windWavesFor(0.0f, {1.0f, 0.0f})) CHECK(w.amplitude == 0.0f);
}

TEST_CASE("the waves are as steep as the sea's in that wind", "[water][wind]") {
    // Cox and Munk's wind part: a mean square slope of 0.00512 U for U m/s.
    CHECK(windWaveSlope(5.0f * kYardsPerMetre) == Catch::Approx(0.160f).margin(0.003f));
    CHECK(windWaveSlope(10.0f * kYardsPerMetre) == Catch::Approx(0.226f).margin(0.003f));
    for (float speed : {1.0f, 3.0f, 6.0f, 12.0f}) {
        const auto waves = windWavesFor(speed, {0.6f, 0.8f});
        CHECK(meanSquareSlope(waves) ==
              Catch::Approx(std::pow(static_cast<double>(windWaveSlope(speed)), 2.0)).epsilon(1e-4));
    }
}

TEST_CASE("a wave runs as fast as water carries one that long", "[water][wind]") {
    // Deep water: a metre-long wave runs at sqrt(g lambda / 2 pi) = 1.25 m/s,
    // and surface tension adds only a fraction of a percent at that length.
    const float k = 2.0f * kPi / (1.0f * kYardsPerMetre);
    const float metresPerSecond = waterWaveAngularSpeed(k) / k / kYardsPerMetre;
    CHECK(metresPerSecond == Catch::Approx(1.25f).margin(0.01f));

    // Long waves outrun short ones down to the centimetre scale.
    float last = 0.0f;
    for (float wavelength = 0.2f; wavelength < 40.0f; wavelength *= 1.5f) {
        const float kw = 2.0f * kPi / wavelength;
        const float speed = waterWaveAngularSpeed(kw) / kw;
        CHECK(speed > last);
        last = speed;
    }
}

TEST_CASE("a stronger wind raises longer waves", "[water][wind]") {
    // The longest train's part of the whole steepness grows with the wind:
    // a light breeze cannot raise thirty-yard waves.
    auto longestShare = [](float speed) {
        const auto waves = windWavesFor(speed, {1.0f, 0.0f});
        // The trains run from the longest down; take the longest quarter.
        double longest = 0.0;
        for (int i = 0; i < kWindWaveCount / 4; ++i) {
            longest += 0.5 * std::pow(static_cast<double>(waves[i].wavenumber * waves[i].amplitude), 2.0);
        }
        return longest / meanSquareSlope(waves);
    };
    CHECK(longestShare(2.0f) < 0.01);
    CHECK(longestShare(8.0f) > 5.0 * longestShare(2.0f));
}

TEST_CASE("the waves run with the wind", "[water][wind]") {
    const glm::vec2 east(1.0f, 0.0f);
    const auto waves = windWavesFor(6.0f, east);
    double downwind = 0.0, upwind = 0.0;
    for (const auto& w : waves) {
        const double slopeSq = 0.5 * std::pow(static_cast<double>(w.wavenumber * w.amplitude), 2.0);
        if (glm::dot(w.direction, east) > 0.0f) downwind += slopeSq; else upwind += slopeSq;
    }
    CHECK(downwind > 4.0 * upwind);
}

TEST_CASE("the water rides along with its waves but never folds over", "[water][wind]") {
    // A gentle sea rides its waves all the way round; a steep one only as far
    // as its surface stays one sheet - the stretch 1 - sum(ride k a) above 0.
    CHECK(windWaveRide(windWavesFor(1.0f, {1.0f, 0.0f})) == 1.0f);
    for (float speed : {2.0f, 5.0f, 10.0f, 20.0f, 40.0f}) {
        const auto waves = windWavesFor(speed, {1.0f, 0.0f});
        double steepness = 0.0;
        for (const auto& w : waves) steepness += static_cast<double>(w.wavenumber * w.amplitude);
        const float ride = windWaveRide(waves);
        CHECK(ride > 0.0f);
        CHECK(ride <= 1.0f);
        CHECK(1.0 - ride * steepness > 0.0);
    }
}

TEST_CASE("a veering wind does not turn the crests", "[water][wind]") {
    // Only how big each train is follows the wind; which way it runs and how
    // long it is stay put, or crests thousands of yards off would race.
    const auto before = windWavesFor(6.0f, {1.0f, 0.0f});
    const auto after = windWavesFor(6.0f, {0.0f, 1.0f});
    for (int i = 0; i < kWindWaveCount; ++i) {
        CHECK(before[i].direction == after[i].direction);
        CHECK(before[i].wavenumber == after[i].wavenumber);
    }
}
