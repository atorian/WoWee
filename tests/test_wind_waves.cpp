// The waves the wind raises on the water: how steep, how fast, which way and
// how long, as the sea has them.
#include <catch_amalgamated.hpp>

#include "rendering/wind_waves.hpp"

#include <glm/geometric.hpp>

#include <cmath>

using wowee::rendering::kWindWaveCount;
using wowee::rendering::waterWaveAngularSpeed;
using wowee::rendering::windWaveRide;
using wowee::rendering::windWaveTrain;
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
        // Waves of ten yards and longer.
        const float tenYards = 2.0f * kPi / 10.0f;
        double longest = 0.0;
        for (const auto& w : waves) {
            if (w.wavenumber <= tenYards) {
                longest += 0.5 * std::pow(static_cast<double>(w.wavenumber * w.amplitude), 2.0);
            }
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

TEST_CASE("long waves keep to the wind, short chop spreads across it", "[water][wind]") {
    // A wind sea's long waves run in long crests close to the wind; its short
    // ones cross each other at wide angles. Measured as how far off the wind
    // the steepness of each lies, on average.
    // A 6 yd/s wind's sea peaks at about twenty yards; the waves near the
    // peak (ten yards and longer) against the ripple of a yard and under.
    const glm::vec2 east(1.0f, 0.0f);
    const auto waves = windWavesFor(6.0f, east);
    const float tenYards = 2.0f * kPi / 10.0f;
    const float oneYard = 2.0f * kPi / 1.0f;
    double longOff = 0.0, longSum = 0.0, shortOff = 0.0, shortSum = 0.0;
    for (const auto& w : waves) {
        const double slopeSq = 0.5 * std::pow(static_cast<double>(w.wavenumber * w.amplitude), 2.0);
        const double off = std::abs(w.direction.y);  // sine of the angle to the wind
        if (w.wavenumber <= tenYards) { longOff += off * slopeSq; longSum += slopeSq; }
        if (w.wavenumber >= oneYard) { shortOff += off * slopeSq; shortSum += slopeSq; }
    }
    REQUIRE(longSum > 0.0);
    REQUIRE(shortSum > 0.0);
    CHECK(longOff / longSum < 0.5 * (shortOff / shortSum));
}

TEST_CASE("the sea follows its wind over its waves' own periods", "[water][wind]") {
    // Still water, then a breeze: the ripple is mostly up within ten seconds,
    // the thirty-yard waves - periods of four seconds - barely begun; given
    // time the sea is the one the wind asks for.
    auto slopeOf = [](const auto& waves, float shortest, float longest) {
        double sum = 0.0;
        for (const auto& w : waves) {
            const float wavelength = 2.0f * kPi / w.wavenumber;
            if (wavelength >= shortest && wavelength <= longest) {
                sum += 0.5 * std::pow(static_cast<double>(w.wavenumber * w.amplitude), 2.0);
            }
        }
        return sum;
    };
    const glm::vec2 east(1.0f, 0.0f);
    const auto steady = windWavesFor(6.0f, east);
    wowee::rendering::WindSea sea;
    sea.advance(0.0, 0.0f, east);            // the sea starts as still air has it
    for (int i = 0; i < 100; ++i) sea.advance(0.1, 6.0f, east);  // ten seconds of breeze
    const auto early = sea.drawn();
    CHECK(slopeOf(early, 0.0f, 0.4f) > 0.6 * slopeOf(steady, 0.0f, 0.4f));
    CHECK(slopeOf(early, 20.0f, 40.0f) < 0.2 * slopeOf(steady, 20.0f, 40.0f));
    for (int i = 0; i < 6000; ++i) sea.advance(0.1, 6.0f, east);  // ten minutes
    CHECK(slopeOf(sea.drawn(), 0.0f, 40.0f) ==
          Catch::Approx(slopeOf(steady, 0.0f, 40.0f)).epsilon(0.01));
}

TEST_CASE("a sea's first moment is the one its wind already has", "[water][wind]") {
    // Logging in by the sea shows a sea, not still water growing.
    const glm::vec2 east(1.0f, 0.0f);
    wowee::rendering::WindSea sea;
    sea.advance(0.016, 6.0f, east);
    CHECK(meanSquareSlope(sea.drawn()) ==
          Catch::Approx(meanSquareSlope(windWavesFor(6.0f, east))).epsilon(1e-6));
}

TEST_CASE("a veering wind does not turn the crests", "[water][wind]") {
    // Only how big each train is, and which are drawn, follow the wind; which
    // way a train runs and how long it is stay put, or crests thousands of
    // yards off would race.
    for (glm::vec2 wind : {glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(-0.6f, 0.8f)}) {
        for (const auto& w : windWavesFor(6.0f, wind)) {
            const auto train = windWaveTrain(w.train);
            CHECK(w.direction == train.direction);
            CHECK(w.wavenumber == train.wavenumber);
            CHECK(w.angularSpeed == train.angularSpeed);
        }
    }
}
