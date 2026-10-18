// The wind's sea as a spectrum (ocean_spectrum.glsli, which the ocean's
// compute shaders run): how steep it is, which way it runs and how long its
// waves are, as the sea has them - summed over the very grids of wave
// vectors the GPU draws them on.
#include <catch_amalgamated.hpp>

#include "rendering/glsl_as_cpp.hpp"
#include "rendering/ocean_sea.hpp"

#include <cmath>

namespace wowee::rendering::glsl {
namespace {
#include "../assets/shaders/ocean_spectrum.glsli"
}
}  // namespace wowee::rendering::glsl

namespace {
using namespace wowee::rendering::glsl;

constexpr float kYardsPerMetre = 1.0936133f;

/// The mean square slope the grids carry in waves between `shortest` and
/// `longest` yards, and of those, how much runs within 90 degrees of
/// `toward` (downwind) and how far off it they run on average (the sine of
/// the angle, weighted by slope).
struct Share {
    double slopeSq = 0.0;
    double downwind = 0.0;
    double offWind = 0.0;
};

Share seaShare(float speed, vec2 toward, float shortest = 0.0f, float longest = 1e9f) {
    Share s;
    const float scale = oceanSlopeScale(speed);
    for (int c = 0; c < kOceanCascades; ++c) {
        for (int y = 0; y < kOceanSize; ++y) {
            for (int x = 0; x < kOceanSize; ++x) {
                const vec2 k = oceanWaveVector(x, y, c);
                const float kl = glm::length(k);
                if (kl <= 0.0f) continue;
                const float wavelength = 2.0f * kOceanPi / kl;
                if (wavelength < shortest || wavelength > longest) continue;
                const double slopeSq = static_cast<double>(kl) * kl *
                                       oceanModeHeightVariance(k, c, toward, speed, scale);
                s.slopeSq += slopeSq;
                if (glm::dot(k, toward) > 0.0f) s.downwind += slopeSq;
                const float along = glm::dot(k / kl, toward);
                s.offWind += slopeSq * std::sqrt(std::max(0.0, 1.0 - static_cast<double>(along) * along));
            }
        }
    }
    return s;
}

}  // namespace

TEST_CASE("still air raises no sea", "[water][ocean]") {
    CHECK(seaShare(0.0f, vec2(1.0f, 0.0f)).slopeSq == 0.0);
}

TEST_CASE("the sea is as steep as Cox and Munk's in its wind", "[water][ocean]") {
    // Their wind part: a mean square slope of 0.00512 U for U m/s - 0.0512
    // in a 10 m/s wind, a slope of 13 degrees.
    CHECK(seaShare(10.0f * kYardsPerMetre, vec2(0.6f, 0.8f)).slopeSq == Catch::Approx(0.0512).epsilon(0.03));
    for (float speed : {2.0f, 4.0f, 8.0f}) {
        CHECK(seaShare(speed, vec2(1.0f, 0.0f)).slopeSq ==
              Catch::Approx(0.00512 * speed / kYardsPerMetre).epsilon(0.03));
    }
}

TEST_CASE("a stronger wind raises longer waves", "[water][ocean]") {
    // Of the whole steepness, what waves of ten yards and longer carry: a
    // light breeze cannot raise them.
    auto longShare = [](float speed) {
        const vec2 east(1.0f, 0.0f);
        return seaShare(speed, east, 10.0f).slopeSq / seaShare(speed, east).slopeSq;
    };
    CHECK(longShare(2.0f) < 0.01);
    CHECK(longShare(8.0f) > 5.0 * longShare(2.0f));
}

TEST_CASE("the sea runs with the wind", "[water][ocean]") {
    for (vec2 toward : {vec2(1.0f, 0.0f), vec2(-0.6f, 0.8f)}) {
        const Share s = seaShare(6.0f, toward);
        CHECK(s.downwind > 4.0 * (s.slopeSq - s.downwind));
    }
}

TEST_CASE("the sea's long waves keep to the wind, its short chop spreads across it", "[water][ocean]") {
    // A 6 yd/s wind's sea peaks near twenty yards: the waves of ten and
    // longer run in long crests close to the wind, those of a yard and under
    // cross each other at wide angles.
    const vec2 east(1.0f, 0.0f);
    const Share longWaves = seaShare(6.0f, east, 10.0f);
    const Share chop = seaShare(6.0f, east, 0.0f, 1.0f);
    REQUIRE(longWaves.slopeSq > 0.0);
    REQUIRE(chop.slopeSq > 0.0);
    // Measured, Donelan and Banner's long waves run some twenty degrees off
    // the wind on average, the chop nearly forty.
    CHECK(longWaves.offWind / longWaves.slopeSq < 0.4);
    CHECK(chop.offWind / chop.slopeSq > 0.55);
}

TEST_CASE("the sea follows its wind over its waves' own periods", "[water][ocean]") {
    // Still water, then a breeze, wave by wave as the GPU steps them: the
    // ripple is mostly up within ten seconds, the thirty-yard waves -
    // periods of four seconds - barely begun; given time the sea is the one
    // the wind asks for.
    const vec2 east(1.0f, 0.0f);
    const float speed = 6.0f;
    const float scale = oceanSlopeScale(speed);
    for (float wavelength : {0.35f, 30.0f}) {
        const vec2 k(2.0f * kOceanPi / wavelength, 0.0f);
        const int c = wavelength < 1.0f ? 2 : 0;
        const float target = oceanModeHeightVariance(k, c, east, speed, scale);
        REQUIRE(target > 0.0f);
        float v = 0.0f;
        for (int frame = 0; frame < 600; ++frame) v = oceanRelaxedVariance(v, target, k.x, 1.0f / 60.0f);
        if (wavelength < 1.0f) CHECK(v > 0.6f * target);
        else CHECK(v < 0.2f * target);
        for (int frame = 0; frame < 36000; ++frame) v = oceanRelaxedVariance(v, target, k.x, 1.0f / 60.0f);
        CHECK(v == Catch::Approx(target).epsilon(0.01));
    }
}

namespace {

/// What the GPU's sea adds up to after `seconds` of a wind of `after`,
/// following a sea that had caught up with a wind of `before`: every wave
/// vector of every grid relaxed as ocean_spectrum.comp relaxes it.
struct GridSea {
    double slopeSq = 0.0;
    double heightVariance = 0.0;
};

GridSea gridSeaAfter(float before, float after, float seconds, vec2 toward) {
    GridSea sea;
    const float scaleBefore = oceanSlopeScale(before);
    const float scaleAfter = oceanSlopeScale(after);
    for (int c = 0; c < kOceanCascades; ++c) {
        for (int y = 0; y < kOceanSize; ++y) {
            for (int x = 0; x < kOceanSize; ++x) {
                const vec2 k = oceanWaveVector(x, y, c);
                const float kl = glm::length(k);
                if (kl <= 0.0f) continue;
                const float was = oceanModeHeightVariance(k, c, toward, before, scaleBefore);
                const float target = oceanModeHeightVariance(k, c, toward, after, scaleAfter);
                const double now = oceanRelaxedVariance(was, target, kl, seconds);
                sea.slopeSq += static_cast<double>(kl) * kl * now;
                sea.heightVariance += now;
            }
        }
    }
    return sea;
}

}  // namespace

TEST_CASE("the sea's steepness and height, as the CPU keeps them, are what the GPU's waves add up to", "[water][ocean]") {
    // A breeze that drops, one that rises: the sea's totals ring by ring
    // follow the wind as its waves on the grids do, one by one.
    const vec2 toward(0.6f, 0.8f);
    struct Change { float before, after, seconds; };
    for (const Change ch : {Change{10.0f, 3.0f, 20.0f}, Change{2.0f, 9.0f, 15.0f}}) {
        wowee::rendering::OceanSea sea;
        sea.advance(0.0, ch.before, toward);
        for (int frame = 0; frame < static_cast<int>(ch.seconds * 30.0f); ++frame) {
            sea.advance(1.0 / 30.0, ch.after, toward);
        }
        const GridSea grid = gridSeaAfter(ch.before, ch.after, ch.seconds, toward);
        CHECK(sea.meanSquareSlope() == Catch::Approx(grid.slopeSq).epsilon(0.05));
        CHECK(sea.heightVariance() == Catch::Approx(grid.heightVariance).epsilon(0.1));
    }
}

TEST_CASE("a sea left by a gale rides its crests less than the breeze after it would", "[water][ocean]") {
    // The water rides toward its crests only as far as the surface stays one
    // sheet, and the sea a gale raised is still standing seconds after it.
    const vec2 east(1.0f, 0.0f);
    wowee::rendering::OceanSea calm;
    calm.advance(0.0, 3.0f, east);
    CHECK(calm.ride() == 1.0f);
    wowee::rendering::OceanSea afterGale;
    afterGale.advance(0.0, 30.0f, east);
    const float inGale = afterGale.ride();
    CHECK(inGale < 1.0f);
    for (int frame = 0; frame < 150; ++frame) afterGale.advance(1.0 / 30.0, 3.0f, east);
    CHECK(afterGale.ride() < 1.0f);
    CHECK(afterGale.ride() >= inGale);
}

TEST_CASE("a wave keeps itself in water half its length deep, and lies down in shallower", "[water][ocean]") {
    // Deep water, for a wave, is water deeper than half its length; a
    // twentieth of it holds little of the wave.
    for (float wavelength : {1.0f, 10.0f, 100.0f}) {
        const float omega = oceanAngularSpeed(2.0f * kOceanPi / wavelength);
        CHECK(oceanDepthFactor(omega, wavelength) == 1.0f);
        CHECK(oceanDepthFactor(omega, 0.5f * wavelength) > 0.95f);
        CHECK(oceanDepthFactor(omega, 0.05f * wavelength) < 0.3f);
        CHECK(oceanDepthFactor(omega, 0.0f) == 0.0f);
        CHECK(oceanDepthFactor(omega, -1.0f) == 0.0f);
    }
}

TEST_CASE("the sea lies down toward the shore, its long waves first", "[water][ocean]") {
    wowee::rendering::OceanSea sea;
    sea.advance(0.0, 8.0f, vec2(1.0f, 0.0f));
    using wowee::rendering::OceanSea;
    const float open = kOceanOpenFetch;
    for (int kind : {OceanSea::kKeptHeight, OceanSea::kKeptSlope}) {
        for (int c = 0; c < kOceanCascades; ++c) {
            // None of it where there is no water under the surface, all of it
            // in the open sea, and never more as the water gets shallower.
            CHECK(sea.kept(kind, c, 0.0f, open) == 0.0f);
            CHECK(sea.kept(kind, c, -2.0f, open) == 0.0f);
            CHECK(sea.kept(kind, c, 500.0f, open) > 0.99f);
            float last = 0.0f;
            for (float depth = 0.0f; depth < 200.0f; depth += 0.1f + depth * 0.05f) {
                const float kept = sea.kept(kind, c, depth, open);
                CHECK(kept >= last - 1e-6f);
                last = kept;
            }
        }
        // Two yards of water: the patch-long swell mostly gone, the chop of
        // under a yard all there.
        const float swell = sea.kept(kind, 0, 2.0f, open);
        const float waves = sea.kept(kind, 1, 2.0f, open);
        const float chop = sea.kept(kind, 2, 2.0f, open);
        CHECK(swell < waves);
        CHECK(waves < chop);
        CHECK(chop > 0.99f);
    }
}

TEST_CASE("a longer fetch lets the wind raise longer waves", "[water][ocean]") {
    // The longest wave a fetch holds half of, for a breeze of 8 yards a
    // second: it grows with the fetch, and at the fetch the open sea
    // needs - tens of miles - every wave is there.
    const float speed = 8.0f;
    auto halfKept = [&](float fetch) {
        float wavelength = 0.05f;
        while (wavelength < 400.0f &&
               oceanFetchFactor(2.0f * kOceanPi / wavelength, speed, fetch) >= 0.5f) {
            wavelength *= 1.05f;
        }
        return wavelength;
    };
    float last = 0.0f;
    for (float fetch : {5.0f, 20.0f, 80.0f, 320.0f, 1280.0f}) {
        const float longest = halfKept(fetch);
        CHECK(longest > last);
        last = longest;
    }
    const float seaFetch = 1.2e4f * speed * speed / kOceanGravity;
    for (float wavelength : {0.3f, 3.0f, 30.0f, 200.0f}) {
        CHECK(oceanFetchFactor(2.0f * kOceanPi / wavelength, speed, seaFetch) == Catch::Approx(1.0f));
    }
    CHECK(oceanFetchFactor(2.0f, speed, 0.0f) == 0.0f);
}

TEST_CASE("a pond keeps its chop and none of the sea's long waves", "[water][ocean]") {
    wowee::rendering::OceanSea sea;
    sea.advance(0.0, 8.0f, vec2(1.0f, 0.0f));
    using wowee::rendering::OceanSea;
    const float deep = 500.0f;
    for (int kind : {OceanSea::kKeptHeight, OceanSea::kKeptSlope}) {
        // Fifty yards from the upwind bank the sea peaks at a third of a
        // yard: the waves of tens of yards cannot have grown there, the
        // short chop has, much of it.
        CHECK(sea.kept(kind, 0, deep, 50.0f) < 0.05f);
        CHECK(sea.kept(kind, 2, deep, 50.0f) > 0.25f);
        CHECK(sea.kept(kind, 2, deep, 50.0f) > 5.0f * sea.kept(kind, 1, deep, 50.0f));
        for (int c = 0; c < kOceanCascades; ++c) {
            CHECK(sea.kept(kind, c, deep, 0.0f) == 0.0f);
            float last = 0.0f;
            for (float fetch = 0.0f; fetch < 5000.0f; fetch += 0.5f + fetch * 0.1f) {
                const float kept = sea.kept(kind, c, deep, fetch);
                CHECK(kept >= last - 1e-6f);
                last = kept;
            }
            CHECK(sea.kept(kind, c, deep, kOceanOpenFetch) > 0.99f);
        }
    }
}

TEST_CASE("each wave runs as fast as water carries one that long, and comes round again", "[water][ocean]") {
    // A metre-long wave runs at sqrt(g lambda / 2 pi) = 1.25 m/s.
    const float metre = 2.0f * kOceanPi / kYardsPerMetre;
    CHECK(oceanAngularSpeed(metre) / metre / kYardsPerMetre == Catch::Approx(1.25f).margin(0.01f));
    // Every wave the grids hold runs within a few percent of its own speed,
    // and after kOceanRepeatSeconds is back where it started, so the sea's
    // clock can go round rather than run on and lose its precision.
    for (int c = 0; c < kOceanCascades; ++c) {
        for (int x = kOceanSize / 2 + 1; x < kOceanSize; x += 3) {
            const float k = glm::length(oceanWaveVector(x, kOceanSize / 2, c));
            const double omega = oceanRepeatingAngularSpeed(k, kOceanRepeatSeconds);
            CHECK(omega == Catch::Approx(oceanAngularSpeed(k)).epsilon(0.025));
            const double turns = omega * kOceanRepeatSeconds / (2.0 * 3.14159265358979);
            CHECK(turns == Catch::Approx(std::round(turns)).margin(1e-3));
        }
    }
}

TEST_CASE("the sea's random draw is a fair one, the same every time", "[water][ocean]") {
    // Independent standard normal numbers: mean 0, variance 1, nothing in
    // common between a cell's pair or between neighbouring cells.
    double sum = 0.0, sumSq = 0.0, pair = 0.0, neighbour = 0.0;
    int n = 0;
    for (int c = 0; c < kOceanCascades; ++c) {
        for (int y = 0; y < kOceanSize; ++y) {
            for (int x = 0; x + 1 < kOceanSize; ++x) {
                const vec2 g = oceanGaussianPair(x, y, c);
                const vec2 next = oceanGaussianPair(x + 1, y, c);
                sum += g.x + g.y;
                sumSq += g.x * g.x + g.y * g.y;
                pair += g.x * g.y;
                neighbour += g.x * next.x;
                n += 1;
            }
        }
    }
    CHECK(sum / (2.0 * n) == Catch::Approx(0.0).margin(0.01));
    CHECK(sumSq / (2.0 * n) == Catch::Approx(1.0).epsilon(0.01));
    CHECK(pair / n == Catch::Approx(0.0).margin(0.01));
    CHECK(neighbour / n == Catch::Approx(0.0).margin(0.01));
    const vec2 again = oceanGaussianPair(17, 200, 1);
    CHECK(again == oceanGaussianPair(17, 200, 1));
}
