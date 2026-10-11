#pragma once

#include "rendering/m2_track_sampler.hpp"

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
    const auto sample = [&](const auto& tracks, uint16_t index) {
        if (index >= tracks.size()) return 1.0f;
        const float value = m2_track::sampleFloat(tracks[index], 0, dayTimeMs,
                                                 elapsedMs, globalDurations, 1.0f);
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
    };
    return sample(colors, colorIndex) * sample(weights, weightIndex);
}

} // namespace wowee::rendering
