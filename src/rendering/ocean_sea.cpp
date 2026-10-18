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

constexpr int kDepths = ocean::kOceanShoalingSteps;
constexpr int kFetches = ocean::kOceanFetchRows;

/// The rings of wavelength and what of them does not change with the wind,
/// worked out once.
struct Rings {
    /// Each ring's wavenumber, at its middle on a log scale from the longest
    /// wave the cascades hold to the shortest, and how wide it is in ln k.
    std::array<float, OceanSea::kRings> k{};
    std::array<float, OceanSea::kRings> width{};
    /// What each keeps at each of the table's depths (oceanDepthFactor).
    std::array<std::array<float, kDepths>, OceanSea::kRings> byDepth{};
    /// The rings in each cascade's band: from first to one before end.
    std::array<int, ocean::kOceanCascades> first{};
    std::array<int, ocean::kOceanCascades> end{};
};

const Rings& rings() {
    static const Rings table = [] {
        Rings r;
        const float lo = std::log(ocean::oceanBandLow(0));
        const float hi = std::log(ocean::oceanBandHigh(ocean::kOceanCascades - 1));
        const float width = (hi - lo) / OceanSea::kRings;
        for (int i = 0; i < OceanSea::kRings; ++i) {
            r.k[i] = std::exp(lo + width * (static_cast<float>(i) + 0.5f));
            r.width[i] = width;
            const float omega = ocean::oceanAngularSpeed(r.k[i]);
            for (int d = 0; d < kDepths; ++d) r.byDepth[i][d] = ocean::oceanDepthFactor(omega, ocean::oceanShoalingDepth(d));
        }
        // The rings rise in k, so a band's are a run of them.
        for (int c = 0; c < ocean::kOceanCascades; ++c) {
            int i = 0;
            while (i < OceanSea::kRings && r.k[i] < ocean::oceanBandLow(c)) ++i;
            r.first[c] = i;
            while (i < OceanSea::kRings && r.k[i] < ocean::oceanBandHigh(c)) ++i;
            r.end[c] = i;
        }
        return r;
    }();
    return table;
}

}  // namespace

void OceanSea::advance(double deltaSeconds, float speedYardsPerSecond, glm::vec2 windDirection) {
    speed_ = std::max(speedYardsPerSecond, 0.0f);
    const float length = glm::length(windDirection);
    if (length > 1e-6f) toward_ = windDirection / length;
    slopeScale_ = ocean::oceanSlopeScale(speed_);

    const Rings& r = rings();
    const float peakK = speed_ > 0.0f ? ocean::kOceanGravity / (speed_ * speed_) : 0.0f;
    for (int i = 0; i < kRings; ++i) {
        // The shape's spread integrates to one round the ring, which leaves
        // the Phillips fall over the ring's width in ln k.
        const float below = peakK / r.k[i];
        const float target = speed_ > 0.0f ? slopeScale_ * std::exp(-below * below) * r.width[i] : 0.0f;
        slopeSq_[i] = started_ ? ocean::oceanRelaxedVariance(slopeSq_[i], target, r.k[i],
                                                             static_cast<float>(deltaSeconds))
                               : target;
    }
    started_ = true;
    time_ = std::fmod(time_ + std::max(deltaSeconds, 0.0), static_cast<double>(ocean::kOceanRepeatSeconds));
    keptCurrent_ = false;
}

static_assert(OceanSea::kKeptEntries ==
                  2 * ocean::kOceanCascades * ocean::kOceanFetchRows * ocean::kOceanShoalingSteps,
              "the table holds a height and a slope share for every cascade at every fetch and depth");

const std::array<glm::vec4, OceanSea::kKeptEntries / 4>& OceanSea::keptTable() const {
    if (!keptCurrent_) tabulateKept();
    return kept_;
}

void OceanSea::tabulateKept() const {
    const Rings& r = rings();
    // What each ring keeps at each fetch in this wind (oceanFetchFactor,
    // with each fetch's peak found once, not once a ring); at each depth it
    // keeps what it always does. Still air raises no sea to limit.
    std::array<std::array<float, kFetches>, kRings> byFetch{};
    const bool wind = speed_ > 0.0f;
    const float openPeak = wind ? ocean::kOceanGravity / (speed_ * speed_) : 0.0f;
    std::array<float, kFetches> fetchPeak{};
    for (int f = 0; f < kFetches; ++f) {
        fetchPeak[f] = wind ? ocean::oceanFetchPeak(speed_, ocean::oceanFetchDistance(f)) : 0.0f;
    }
    for (int i = 0; i < kRings; ++i) {
        for (int f = 0; f < kFetches; ++f) byFetch[i][f] = ocean::oceanFetchKept(r.k[i], openPeak, fetchPeak[f]);
    }
    for (int kind = 0; kind < 2; ++kind) {
        // Each ring weighted by what it holds: its slope, or its height
        // (slope over k squared).
        std::array<double, kRings> weight{};
        for (int i = 0; i < kRings; ++i) {
            weight[i] = kind == kKeptSlope ? slopeSq_[i] : slopeSq_[i] / (r.k[i] * r.k[i]);
        }
        for (int c = 0; c < ocean::kOceanCascades; ++c) {
            double whole = 0.0;
            for (int i = r.first[c]; i < r.end[c]; ++i) whole += weight[i];
            for (int f = 0; f < kFetches; ++f) {
                for (int d = 0; d < kDepths; ++d) {
                    double keptEnergy = 0.0;
                    for (int i = r.first[c]; i < r.end[c]; ++i) keptEnergy += weight[i] * r.byDepth[i][d] * byFetch[i][f];
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
    keptCurrent_ = true;
}

float OceanSea::kept(int kind, int cascade, float depth, float fetch) const {
    const auto& table = keptTable();
    const glm::vec2 depthSlot = ocean::oceanShoalingSlot(depth);
    const glm::vec2 fetchSlot = ocean::oceanFetchSlot(fetch);
    const int base = (kind * ocean::kOceanCascades + cascade) * ocean::kOceanFetchRows;
    // A step below the first is no depth, or no fetch: nothing kept.
    const auto at = [&](int row, int step) {
        if (row < 0 || step < 0) return 0.0f;
        const int entry = (base + row) * kDepths + step;
        return table[entry / 4][entry % 4];
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
    const Rings& r = rings();
    for (int i = 0; i < kRings; ++i) sum += slopeSq_[i] / (r.k[i] * r.k[i]);
    return sum;
}

float OceanSea::ride() const {
    return ocean::oceanRide(meanSquareSlope());
}

} // namespace rendering
} // namespace wowee
