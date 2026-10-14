#pragma once

#include "rendering/m2_batch_opacity.hpp"

namespace wowee::rendering {

// Sky Stand tracks span a whole day. Cloud scrolling/global fades retain
// their own elapsed-time clocks instead of following the day/night phase.
inline float skyAnimationTimeMs(float hours, uint32_t duration) {
    if (!std::isfinite(hours)) hours = 12.0f;
    const float wrapped = hours - std::floor(hours / 24.0f) * 24.0f;
    return wrapped / 24.0f * static_cast<float>(duration);
}

inline float skyBatchOpacity(
    const std::vector<pipeline::M2AnimationTrack>& colors, uint16_t colorIndex,
    const std::vector<pipeline::M2AnimationTrack>& weights, uint16_t weightIndex,
    float dayTimeMs, float elapsedMs, const std::vector<uint32_t>& globalDurations) {
    return m2SampleBatchOpacity(colors, colorIndex, weights, weightIndex,
                                0, dayTimeMs, elapsedMs, globalDurations);
}

} // namespace wowee::rendering
