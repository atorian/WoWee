// The water's answer to what moves through it (water_wake.glsli, which the
// wake's compute shaders run): its waves run at water's own speed, a body at
// rest sits in a hollow and raises none, and one on the move leaves Kelvin's
// wedge behind it with waves as long as its speed makes them.
#include <catch_amalgamated.hpp>

#include "rendering/glsl_as_cpp.hpp"

#include <cmath>
#include <vector>

namespace wowee::rendering::glsl {
namespace {
#include "../assets/shaders/ocean_spectrum.glsli"
#include "../assets/shaders/ocean_fft.glsli"
#include "../assets/shaders/water_wake.glsli"
}
}  // namespace wowee::rendering::glsl

namespace {
using namespace wowee::rendering::glsl;

/// The field stepped as WaterWake steps it, on `n` texels a side: the
/// height and pressure head through the FFT, times the vertical derivative,
/// back, and each texel stepped and soaked at the edge.
struct Field {
    int n;
    bool edge;
    std::vector<float> h, previous;
    std::vector<float> downward;  // the water's downward acceleration in the last step

    Field(int size, bool soakEdge)
        : n(size), edge(soakEdge), h(size * size, 0.0f), previous(size * size, 0.0f), downward(size * size, 0.0f) {}

    /// One line of complex numbers through the Stockham inverse FFT.
    static void inverseLine(std::vector<vec2>& line) {
        const int len = static_cast<int>(line.size());
        std::vector<vec2> other(len);
        std::vector<vec2>* from = &line;
        std::vector<vec2>* to = &other;
        for (int stage = 0; (1 << stage) < len; ++stage) {
            for (int j = 0; j < len / 2; ++j) {
                const OceanButterfly b = oceanStockhamButterfly(j, stage, len);
                const vec2 s0 = (*from)[b.source0];
                const vec2 w = oceanComplexMul(b.twiddle, (*from)[b.source1]);
                (*to)[b.target0] = s0 + w;
                (*to)[b.target1] = s0 - w;
            }
            std::swap(from, to);
        }
        if (from != &line) line = *from;
    }
    void inverse2d(std::vector<vec2>& grid) const {
        std::vector<vec2> line(n);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) line[x] = grid[y * n + x];
            inverseLine(line);
            for (int x = 0; x < n; ++x) grid[y * n + x] = line[x];
        }
        for (int x = 0; x < n; ++x) {
            for (int y = 0; y < n; ++y) line[y] = grid[y * n + x];
            inverseLine(line);
            for (int y = 0; y < n; ++y) grid[y * n + x] = line[y];
        }
    }

    /// `pressure` is the head on each texel this step.
    void step(const std::vector<float>& pressure) {
        // Forward, as the shaders take it: the inverse of the conjugate,
        // conjugated.
        std::vector<vec2> spectrum(n * n);
        for (int i = 0; i < n * n; ++i) spectrum[i] = vec2(h[i] + (pressure.empty() ? 0.0f : pressure[i]), 0.0f);
        inverse2d(spectrum);  // conj(x) of a real x is x
        const float norm = 1.0f / static_cast<float>(n * n);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const vec2 k = wakeWaveVector({x, y}, n, kWakeTexelYards);
                vec2& s = spectrum[y * n + x];
                s = oceanConjugate(s) * (norm * wakeVerticalDerivative(length(k)));
            }
        }
        inverse2d(spectrum);
        std::vector<float> next(n * n);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const int i = y * n + x;
                float v = wakeStep(h[i], previous[i], spectrum[i].x, kWakeStepSeconds);
                if (edge) v *= wakeEdgeKeep({x, y}, n);
                next[i] = v;
                downward[i] = kOceanGravity * spectrum[i].x;
            }
        }
        previous = h;
        h = next;
    }

    float at(glm::vec2 yards) const {
        const glm::ivec2 t = glm::ivec2(glm::floor(yards / kWakeTexelYards));
        return h[((t.y % n + n) % n) * n + ((t.x % n + n) % n)];
    }
};

constexpr float kPiF = 3.14159265f;

}  // namespace

TEST_CASE("the wake's waves run at water's own speed", "[water][wake]") {
    // A standing wave, let go: it swings through flat a quarter of its
    // period on, which water's dispersion sets - long waves slower to swing
    // than short ones, as they run faster.
    const int n = 128;
    const float domain = n * kWakeTexelYards;
    for (int crests : {32, 8, 3}) {
        const float k = 2.0f * kPiF * crests / domain;
        Field f(n, false);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                f.h[y * n + x] = 0.1f * std::cos(k * (x + 0.5f) * kWakeTexelYards);
            }
        }
        f.previous = f.h;
        // Water's own angular speed, slowed a little by the wake's damping:
        // a damped swing from rest passes through nought a touch late.
        const float omega = std::sqrt(kOceanGravity * k + kOceanTension * k * k * k);
        const float damped = std::sqrt(omega * omega - 0.25f * kWakeDamping * kWakeDamping);
        const float throughNought = (0.5f * kPiF + std::atan(kWakeDamping / (2.0f * damped))) / damped;
        int steps = 0;
        const float start = f.h[0];
        while (f.h[0] * start > 0.0f && steps < 10000) {
            f.step({});
            ++steps;
        }
        const float quarter = steps * kWakeStepSeconds;
        CHECK(quarter == Catch::Approx(throughNought).epsilon(0.04));
    }
}

TEST_CASE("a body at rest sits in a hollow and raises no waves", "[water][wake]") {
    // A head of a fifth of a yard held on the water: once the first swell
    // has died away the water under it is that much down, and still.
    const int n = 64;
    Field f(n, true);
    std::vector<float> pressure(n * n);
    const vec2 centre(16.0f, 16.0f);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const vec2 p = (vec2(x, y) + 0.5f) * kWakeTexelYards;
            pressure[y * n + x] = wakePressure(p, centre, vec2(0.0f), 1.0f, 0.6f, 0.2f);
        }
    }
    for (int i = 0; i < 60 * 30; ++i) f.step(pressure);
    // Down by the head on it, texel by texel.
    for (vec2 at : {centre, centre + vec2(0.6f, 0.2f), centre + vec2(-0.3f, -0.4f)}) {
        const glm::ivec2 t = glm::ivec2(glm::floor(at / kWakeTexelYards));
        CHECK(f.h[t.y * n + t.x] == Catch::Approx(-pressure[t.y * n + t.x]).margin(0.01f));
    }
    CHECK(f.at(centre) < -0.1f);
    CHECK(std::abs(f.at(centre + vec2(10.0f, 0.0f))) < 0.005f);
    const auto before = f.h;
    f.step(pressure);
    float moved = 0.0f;
    for (int i = 0; i < n * n; ++i) moved = std::max(moved, std::abs(f.h[i] - before[i]));
    CHECK(moved < 1e-3f);
}

namespace {

/// A body crossing the field along +x at `speed`, from the west edge, for
/// `seconds`: where it ended, the field it left, and the foam on it, made
/// as the surface pass makes it.
struct Run {
    Field field;
    vec2 at;
    std::vector<float> foam;

    float foamAt(vec2 yards) const {
        const glm::ivec2 t = glm::ivec2(glm::floor(yards / kWakeTexelYards));
        return foam[t.y * field.n + t.x];
    }
};

Run swimAcross(float speed, float seconds, int n) {
    Field f(n, true);
    const float domain = n * kWakeTexelYards;
    vec2 body(0.15f * domain, 0.5f * domain);
    std::vector<float> pressure(n * n), foam(n * n, 0.0f);
    const int steps = static_cast<int>(seconds / kWakeStepSeconds);
    for (int s = 0; s < steps; ++s) {
        const vec2 was = body;
        body.x += speed * kWakeStepSeconds;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const vec2 p = (vec2(x, y) + 0.5f) * kWakeTexelYards;
                pressure[y * n + x] = wakePressure(p, body, vec2(1.0f, 0.0f), 0.8f, 0.4f, 0.2f);
            }
        }
        f.step(pressure);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const vec2 p = (vec2(x, y) + 0.5f) * kWakeTexelYards;
                const float churn = wakeChurnAt(p, was, body, vec2(1.0f, 0.0f), 0.8f, 0.4f, speed);
                float& here = foam[y * n + x];
                here = wakeFoam(here, churn, f.downward[y * n + x], kWakeStepSeconds);
            }
        }
    }
    return {f, body, foam};
}

}  // namespace

TEST_CASE("a swimmer leaves white water behind it that fades, a body at rest none", "[water][wake]") {
    // Right behind it the water it has just churned; a few yards back -
    // a second or two old - less of it; beside its track, none.
    const Run swim = swimAcross(3.0f, 6.0f, 128);
    const float justBehind = swim.foamAt(swim.at - vec2(1.0f, 0.0f));
    const float older = swim.foamAt(swim.at - vec2(8.0f, 0.0f));
    CHECK(justBehind > 0.5f);
    CHECK(older < 0.6f * justBehind);
    CHECK(older > 0.05f);
    CHECK(swim.foamAt(swim.at - vec2(4.0f, 4.0f)) < 0.05f);

    const Run still = swimAcross(0.0f, 6.0f, 128);
    float most = 0.0f;
    for (float v : still.foam) most = std::max(most, v);
    CHECK(most < 0.01f);
}

TEST_CASE("a body on the move leaves Kelvin's wedge behind it", "[water][wake]") {
    // At distances behind it, how far out to either side the wake reaches:
    // the angle that makes with the track is Kelvin's, 19.5 degrees, for
    // any speed - not a cone that narrows as the body speeds up.
    for (float speed : {2.5f, 4.0f}) {
        const Run run = swimAcross(speed, 9.0f, 128);
        float wedge = 0.0f;
        int measured = 0;
        for (float behind = 10.0f; behind <= 18.0f; behind += 2.0f) {
            float peak = 0.0f;
            std::vector<float> across;
            for (float side = -20.0f; side <= 20.0f; side += 0.25f) {
                const float h = std::abs(run.field.at(run.at + vec2(-behind, side)));
                across.push_back(h);
                peak = std::max(peak, h);
            }
            if (peak <= 1e-4f) continue;
            float reach = 0.0f;
            for (size_t i = 0; i < across.size(); ++i) {
                if (across[i] > 0.25f * peak) reach = std::max(reach, std::abs(-20.0f + 0.25f * static_cast<float>(i)));
            }
            wedge += std::atan2(reach, behind);
            ++measured;
        }
        REQUIRE(measured > 2);
        const float degrees = wedge / measured * 180.0f / kPiF;
        CHECK(degrees > 13.0f);
        CHECK(degrees < 26.0f);
    }
}

TEST_CASE("the faster a body goes, the longer the waves it leaves", "[water][wake]") {
    // The waves along its track: as long as the waves that run at its
    // speed, 2 pi U^2 / g, go - the transverse ones - and a little shorter
    // for the diverging ones crossing them, which a body as long as it is
    // wide raises as much of. So: longer as it goes faster, by about the
    // square of its speed, and of that length.
    std::vector<float> lengths;
    for (float speed : {2.5f, 4.0f}) {
        const Run run = swimAcross(speed, 9.0f, 128);
        std::vector<float> crossings;
        float last = run.field.at(run.at + vec2(-3.0f, 0.0f));
        for (float behind = 3.0f; behind <= 26.0f; behind += 0.1f) {
            const float h = run.field.at(run.at + vec2(-behind, 0.0f));
            if ((h > 0.0f) != (last > 0.0f)) crossings.push_back(behind);
            last = h;
        }
        REQUIRE(crossings.size() >= 3);
        const float wavelength = 2.0f * (crossings.back() - crossings.front()) / (crossings.size() - 1);
        const float transverse = 2.0f * kPiF * speed * speed / kOceanGravity;
        CHECK(wavelength > 0.5f * transverse);
        CHECK(wavelength < 1.2f * transverse);
        lengths.push_back(wavelength);
    }
    CHECK(lengths[1] > 1.8f * lengths[0]);
}
