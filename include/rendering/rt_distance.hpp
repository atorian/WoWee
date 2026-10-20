#pragma once
#include <algorithm>
#include <cmath>

namespace wowee::rendering {

inline constexpr float kMinRtDistance = 100.0f;
inline constexpr float kMaxRtDistance = 1500.0f;
inline constexpr float kDefaultRtDistance = 400.0f;
inline float clampRtDistance(float value) {
    return std::isfinite(value) ? std::clamp(value, kMinRtDistance, kMaxRtDistance) : kDefaultRtDistance;
}


} // namespace wowee::rendering
