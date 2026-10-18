#include "rendering/weather_mix.hpp"

#include <cmath>

namespace wowee {
namespace rendering {

using pipeline::WeatherKind;

WeatherMix weatherMixOf(const WeatherTarget& target) {
    const float i = std::clamp(target.intensity, 0.0f, 1.0f);
    WeatherMix m;
    switch (target.kind) {
        case WeatherKind::Clear: break;
        case WeatherKind::Fog: m.mist = i; break;
        case WeatherKind::Rain: m.rain = i; break;
        case WeatherKind::Snow: m.snow = i; break;
        case WeatherKind::Sandstorm: m.sand = i; break;
        case WeatherKind::Thunderstorm:
            m.rain = i;
            m.thunder = i;
            break;
        case WeatherKind::BlackRain:
            m.rain = i;
            m.dark = 1.0f;
            break;
        case WeatherKind::BlackSnow:
            m.snow = i;
            m.dark = 1.0f;
            break;
    }
    return m;
}

namespace {

float toward(float from, float to, float step) {
    return from < to ? std::min(from + step, to) : std::max(from - step, to);
}

}  // namespace

void WeatherBlend::update(float dt, const WeatherTarget& target) {
    const WeatherMix goal = weatherMixOf(target);
    const bool arrivesAtOnce = !started_ || (target.abrupt && !(target == target_)) ||
                               transitionSeconds_ <= 0.0f;
    target_ = target;
    started_ = true;
    if (arrivesAtOnce) {
        mix_ = goal;
        return;
    }
    const float step = std::max(dt, 0.0f) / transitionSeconds_;
    mix_.rain = toward(mix_.rain, goal.rain, step);
    mix_.snow = toward(mix_.snow, goal.snow, step);
    mix_.sand = toward(mix_.sand, goal.sand, step);
    mix_.mist = toward(mix_.mist, goal.mist, step);
    mix_.thunder = toward(mix_.thunder, goal.thunder, step);
    // What falls goes dark or pale with it, not on its own: black rain turning
    // to clear stays black as it thins out.
    if (goal.rain + goal.snow > 0.0f) mix_.dark = toward(mix_.dark, goal.dark, step);
}

}  // namespace rendering
}  // namespace wowee
