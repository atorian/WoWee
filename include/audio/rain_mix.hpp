#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace wowee::audio {

// Gains for the light/medium/heavy outdoor loops followed by their muffled
// indoor versions. No device dependency: the same envelope drives playback.
class RainMix {
public:
    const std::array<float, 6>& update(float dt, float amount, bool indoors) {
        std::array<float, 6> target{};
        amount = std::clamp(amount, 0.0f, 1.0f);
        const float level = amount * 3.0f;
        const float gain = indoors ? 0.09f : 0.4f;
        const size_t offset = indoors ? 3 : 0;
        target[offset] = std::clamp(1.0f - std::abs(level - 1.0f), 0.0f, 1.0f) * gain;
        target[offset + 1] = std::clamp(1.0f - std::abs(level - 2.0f), 0.0f, 1.0f) * gain;
        target[offset + 2] = std::clamp(level - 2.0f, 0.0f, 1.0f) * gain;
        // Roughly two seconds to settle at a doorway; independent of frame rate.
        const float blend = -std::expm1(-std::max(dt, 0.0f) / 0.65f);
        for (size_t i = 0; i < gains_.size(); ++i) {
            gains_[i] += (target[i] - gains_[i]) * blend;
        }
        return gains_;
    }
private:
    std::array<float, 6> gains_{};
};

} // namespace wowee::audio
