#include "rendering/ocean_sea.hpp"
#include "rendering/glsl_as_cpp.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace wowee::rendering::glsl {
namespace {
// The shaders' whole include, of which this takes what the sea's totals need.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../assets/shaders/ocean_spectrum.glsli"
#pragma GCC diagnostic pop
}
}  // namespace wowee::rendering::glsl

namespace wowee {
namespace rendering {

namespace {

namespace ocean = glsl;

/// Ring `ring`'s wavenumber, at its middle on a log scale from the longest
/// wave the cascades hold to the shortest, and how wide it is in ln k.
struct Ring {
    float k;
    float width;
};

Ring ringAt(int ring) {
    const float lo = std::log(ocean::oceanBandLow(0));
    const float hi = std::log(ocean::oceanBandHigh(ocean::kOceanCascades - 1));
    const float width = (hi - lo) / OceanSea::kRings;
    return {std::exp(lo + width * (static_cast<float>(ring) + 0.5f)), width};
}

}  // namespace

void OceanSea::advance(double deltaSeconds, float speedYardsPerSecond, glm::vec2 windDirection) {
    speed_ = std::max(speedYardsPerSecond, 0.0f);
    const float length = glm::length(windDirection);
    if (length > 1e-6f) toward_ = windDirection / length;
    slopeScale_ = ocean::oceanSlopeScale(speed_);

    const float peakK = speed_ > 0.0f ? ocean::kOceanGravity / (speed_ * speed_) : 0.0f;
    for (int i = 0; i < kRings; ++i) {
        const Ring r = ringAt(i);
        // The shape's spread integrates to one round the ring, which leaves
        // the Phillips fall over the ring's width in ln k.
        const float below = peakK / r.k;
        const float target = speed_ > 0.0f ? slopeScale_ * std::exp(-below * below) * r.width : 0.0f;
        slopeSq_[i] = started_ ? ocean::oceanRelaxedVariance(slopeSq_[i], target, r.k,
                                                             static_cast<float>(deltaSeconds))
                               : target;
    }
    started_ = true;
    time_ = std::fmod(time_ + std::max(deltaSeconds, 0.0), static_cast<double>(ocean::kOceanRepeatSeconds));
    tabulateKept();
}

static_assert(OceanSea::kKeptEntries ==
                  2 * ocean::kOceanCascades * ocean::kOceanFetchRows * ocean::kOceanShoalingSteps,
              "the table holds a height and a slope share for every cascade at every fetch and depth");

void OceanSea::tabulateKept() {
    constexpr int kDepths = ocean::kOceanShoalingSteps;
    constexpr int kFetches = ocean::kOceanFetchRows;
    // What each ring keeps at each depth and each fetch, apart.
    std::array<std::array<float, kDepths>, kRings> byDepth{};
    std::array<std::array<float, kFetches>, kRings> byFetch{};
    for (int i = 0; i < kRings; ++i) {
        const Ring r = ringAt(i);
        const float omega = ocean::oceanAngularSpeed(r.k);
        for (int d = 0; d < kDepths; ++d) byDepth[i][d] = ocean::oceanDepthFactor(omega, ocean::oceanShoalingDepth(d));
        for (int f = 0; f < kFetches; ++f) {
            byFetch[i][f] = ocean::oceanFetchFactor(r.k, speed_, ocean::oceanFetchDistance(f));
        }
    }
    for (int kind = 0; kind < 2; ++kind) {
        for (int c = 0; c < ocean::kOceanCascades; ++c) {
            for (int f = 0; f < kFetches; ++f) {
                for (int d = 0; d < kDepths; ++d) {
                    // Each ring of the cascade's band, weighted by what it
                    // holds: its slope, or its height (slope over k squared).
                    double keptEnergy = 0.0, whole = 0.0;
                    for (int i = 0; i < kRings; ++i) {
                        const Ring r = ringAt(i);
                        if (r.k < ocean::oceanBandLow(c) || r.k >= ocean::oceanBandHigh(c)) continue;
                        const double weight = kind == kKeptSlope ? slopeSq_[i] : slopeSq_[i] / (r.k * r.k);
                        whole += weight;
                        keptEnergy += weight * byDepth[i][d] * byFetch[i][f];
                    }
                    // Energy kept, as a share of amplitude: its root. A
                    // cascade that holds nothing in this wind keeps all of
                    // its nothing.
                    const float share = whole > 0.0 ? static_cast<float>(std::sqrt(keptEnergy / whole)) : 1.0f;
                    const int entry = ((kind * ocean::kOceanCascades + c) * kFetches + f) * kDepths + d;
                    kept_[entry / 4][entry % 4] = share;
                }
            }
        }
    }
}

float OceanSea::kept(int kind, int cascade, float depth, float fetch) const {
    constexpr int kDepths = ocean::kOceanShoalingSteps;
    const glm::vec2 depthSlot = ocean::oceanShoalingSlot(depth);
    const glm::vec2 fetchSlot = ocean::oceanFetchSlot(fetch);
    const int base = (kind * ocean::kOceanCascades + cascade) * ocean::kOceanFetchRows;
    // A step below the first is no depth, or no fetch: nothing kept.
    const auto at = [&](int row, int step) {
        if (row < 0 || step < 0) return 0.0f;
        const int entry = (base + row) * kDepths + step;
        return kept_[entry / 4][entry % 4];
    };
    const int f = static_cast<int>(fetchSlot.x);
    const int d = static_cast<int>(depthSlot.x);
    const float nearRow = at(f, d) + (at(f, d + 1) - at(f, d)) * depthSlot.y;
    const float farRow = at(f + 1, d) + (at(f + 1, d + 1) - at(f + 1, d)) * depthSlot.y;
    return nearRow + (farRow - nearRow) * fetchSlot.y;
}

float OceanSea::meanSquareSlope() const {
    float sum = 0.0f;
    for (float s : slopeSq_) sum += s;
    return sum;
}

float OceanSea::heightVariance() const {
    float sum = 0.0f;
    for (int i = 0; i < kRings; ++i) {
        const float k = ringAt(i).k;
        sum += slopeSq_[i] / (k * k);
    }
    return sum;
}

float OceanSea::ride() const {
    return ocean::oceanRide(meanSquareSlope());
}

} // namespace rendering
} // namespace wowee
