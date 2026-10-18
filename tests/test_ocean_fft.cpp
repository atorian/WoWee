// The sea's surface out of its spectrum, by the very functions the ocean's
// compute shaders run (ocean_fft.glsli): the inverse FFT is the inverse
// transform, a single wave comes out as the trochoid the water makes of it,
// and a whole drawn sea is as steep as its spectrum says and comes round
// again after its repeat.
#include <catch_amalgamated.hpp>

#include "rendering/glsl_as_cpp.hpp"

#include <cmath>
#include <random>
#include <vector>

namespace wowee::rendering::glsl {
namespace {
#include "../assets/shaders/ocean_spectrum.glsli"
#include "../assets/shaders/ocean_fft.glsli"
}
}  // namespace wowee::rendering::glsl

namespace {
using namespace wowee::rendering::glsl;

constexpr double kPiD = 3.14159265358979323846;

/// One line of points, two complex lanes to a vec4, through every stage of
/// the Stockham butterflies, as ocean_fft.comp takes a row in shared memory.
void inverseFftLine(std::vector<vec4>& line) {
    const int n = static_cast<int>(line.size());
    std::vector<vec4> other(n);
    std::vector<vec4>* from = &line;
    std::vector<vec4>* to = &other;
    for (int stage = 0; (1 << stage) < n; ++stage) {
        for (int j = 0; j < n / 2; ++j) {
            const OceanButterfly b = oceanStockhamButterfly(j, stage, n);
            const vec4 s0 = (*from)[b.source0];
            const vec4 s1 = (*from)[b.source1];
            const vec2 w0 = oceanComplexMul(b.twiddle, vec2(s1.x, s1.y));
            const vec2 w1 = oceanComplexMul(b.twiddle, vec2(s1.z, s1.w));
            const vec4 w(w0.x, w0.y, w1.x, w1.y);
            (*to)[b.target0] = s0 + w;
            (*to)[b.target1] = s0 - w;
        }
        std::swap(from, to);
    }
    if (from != &line) line = *from;
}

/// Rows, then columns, of a kOceanSize square grid (index y * size + x).
void inverseFft2d(std::vector<vec4>& grid) {
    const int n = kOceanSize;
    std::vector<vec4> line(n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) line[x] = grid[y * n + x];
        inverseFftLine(line);
        for (int x = 0; x < n; ++x) grid[y * n + x] = line[x];
    }
    for (int x = 0; x < n; ++x) {
        for (int y = 0; y < n; ++y) line[y] = grid[y * n + x];
        inverseFftLine(line);
        for (int y = 0; y < n; ++y) grid[y * n + x] = line[y];
    }
}

int mirrorIndex(int m) { return (kOceanSize - m) % kOceanSize; }

/// A sea on `cascade`'s grid at `time`, out of its initial heights `h0`
/// (index y * size + x), evolved and transformed as the GPU does it.
std::vector<OceanSurface> seaAt(const std::vector<vec2>& h0, int cascade, float time, float ride) {
    const int n = kOceanSize;
    std::vector<vec4> a(n * n), b(n * n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const vec2 k = oceanWaveVector(x, y, cascade);
            const float omega = oceanRepeatingAngularSpeed(length(k), kOceanRepeatSeconds);
            const OceanModes m = oceanEvolve(k, h0[y * n + x], h0[mirrorIndex(y) * n + mirrorIndex(x)],
                                             omega, time, ride);
            a[y * n + x] = m.modesA;
            b[y * n + x] = m.modesB;
        }
    }
    inverseFft2d(a);
    inverseFft2d(b);
    std::vector<OceanSurface> surface(n * n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            surface[y * n + x] = oceanSurfaceAt(a[y * n + x], b[y * n + x], oceanGridSign(x, y));
        }
    }
    return surface;
}

}  // namespace

TEST_CASE("the ocean's FFT is the inverse Fourier transform", "[water][ocean]") {
    std::mt19937 rng(7);
    std::normal_distribution<float> normal;
    for (int n : {8, 256}) {
        std::vector<vec4> input(n);
        for (vec4& v : input) v = vec4(normal(rng), normal(rng), normal(rng), normal(rng));
        std::vector<vec4> fast = input;
        inverseFftLine(fast);
        double worst = 0.0, largest = 0.0;
        for (int x = 0; x < n; ++x) {
            double re0 = 0, im0 = 0, re1 = 0, im1 = 0;
            for (int m = 0; m < n; ++m) {
                const double angle = 2.0 * kPiD * m * x / n;
                const double c = std::cos(angle), s = std::sin(angle);
                re0 += input[m].x * c - input[m].y * s;
                im0 += input[m].x * s + input[m].y * c;
                re1 += input[m].z * c - input[m].w * s;
                im1 += input[m].z * s + input[m].w * c;
            }
            for (double d : {fast[x].x - re0, fast[x].y - im0, fast[x].z - re1, fast[x].w - im1}) {
                worst = std::max(worst, std::abs(d));
            }
            largest = std::max({largest, std::abs(re0), std::abs(im0), std::abs(re1), std::abs(im1)});
        }
        CHECK(worst < 1e-5 * largest * std::log2(n) + 1e-6);
    }
}

TEST_CASE("a single wave comes out as the water makes it: a trochoid running with its wind", "[water][ocean]") {
    // One wave on cascade 1's grid, five cells along x: a crest-to-mean height
    // of a, running toward +x. The water rides toward each crest, so the
    // surface is a trochoid: sharp crests, wide troughs.
    const int cascade = 1;
    const int n = kOceanSize;
    const float a = 0.3f;
    const float ride = 1.0f;
    std::vector<vec2> h0(n * n, vec2(0.0f));
    const int cellX = n / 2 + 5, cellY = n / 2;
    h0[cellY * n + cellX] = vec2(0.5f * a, 0.0f);
    const float k = length(oceanWaveVector(cellX, cellY, cascade));
    const float omega = oceanRepeatingAngularSpeed(k, kOceanRepeatSeconds);
    const float yardsPerCell = oceanPatch(cascade) / n;

    for (float time : {0.0f, 0.7f}) {
        const auto sea = seaAt(h0, cascade, time, ride);
        double worstHeight = 0.0, worstRide = 0.0, worstSlope = 0.0;
        for (int x = 1; x + 1 < n; ++x) {
            const float px = x * yardsPerCell;
            const float theta = k * px - omega * time;
            const OceanSurface& s = sea[x];
            // The water at rest position px has gone to px + Dx and is at
            // height H: a cos and, toward the crest ahead, -a sin.
            worstHeight = std::max(worstHeight, static_cast<double>(std::abs(s.displacement.z - a * std::cos(theta))));
            worstRide = std::max(worstRide, static_cast<double>(std::abs(s.displacement.x + a * ride * std::sin(theta))));
            CHECK(std::abs(s.displacement.y) < 1e-4f);
            // The slope the light is shaped by is the carried surface's own:
            // its rise over its run between the neighbouring points.
            const OceanSurface& before = sea[x - 1];
            const OceanSurface& after = sea[x + 1];
            const float run = (after.displacement.x + yardsPerCell) - (before.displacement.x - yardsPerCell);
            const float rise = after.displacement.z - before.displacement.z;
            const float measured = rise / run;
            worstSlope = std::max(worstSlope, static_cast<double>(std::abs(s.slope.x - measured) /
                                                                  (std::abs(measured) + a * k)));
        }
        CHECK(worstHeight < 1e-4);
        CHECK(worstRide < 1e-4);
        CHECK(worstSlope < 0.02);
    }

    // Sharp crests, wide troughs: on the carried surface, the water stands
    // above its mean height over less than half of each wavelength.
    // Each point stands for the stretch of carried surface between it and
    // the next, its cell times 1 + Dxx.
    const auto sea = seaAt(h0, cascade, 0.0f, ride);
    double span = 0.0, aboveWidth = 0.0;
    for (int x = 0; x < n; ++x) {
        const double width = yardsPerCell * (1.0 + sea[x].derivatives.z);
        span += width;
        if (sea[x].displacement.z > 0.0f) aboveWidth += width;
    }
    CHECK(aboveWidth < 0.45 * span);
}

TEST_CASE("a sea drawn from its spectrum is as steep as the spectrum, and comes round again", "[water][ocean]") {
    const int cascade = 1;
    const int n = kOceanSize;
    const vec2 toward(0.8f, 0.6f);
    const float speed = 8.0f;
    const float scale = oceanSlopeScale(speed);
    std::vector<vec2> h0(n * n);
    double spectrumSlopeSq = 0.0;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const vec2 k = oceanWaveVector(x, y, cascade);
            const float variance = oceanModeHeightVariance(k, cascade, toward, speed, scale);
            spectrumSlopeSq += static_cast<double>(dot(k, k)) * variance;
            h0[y * n + x] = oceanInitialHeight(oceanGaussianPair(x, y, cascade), variance);
        }
    }
    REQUIRE(spectrumSlopeSq > 0.0);
    const auto sea = seaAt(h0, cascade, 3.7f, 1.0f);
    double slopeSq = 0.0, height = 0.0;
    for (const OceanSurface& s : sea) {
        slopeSq += s.derivatives.x * s.derivatives.x + s.derivatives.y * s.derivatives.y;
        height += s.displacement.z;
    }
    slopeSq /= sea.size();
    height /= sea.size();
    // One draw of tens of thousands of waves: within a few percent.
    CHECK(slopeSq == Catch::Approx(spectrumSlopeSq).epsilon(0.05));
    CHECK(std::abs(height) < 1e-4);

    const auto later = seaAt(h0, cascade, 3.7f + kOceanRepeatSeconds, 1.0f);
    double worst = 0.0;
    for (size_t i = 0; i < sea.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(sea[i].displacement.z - later[i].displacement.z)));
    }
    CHECK(worst < 1e-3);
}
