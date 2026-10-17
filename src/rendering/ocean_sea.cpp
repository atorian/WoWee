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
