#pragma once

#include "rendering/weather_mix.hpp"
#include "rendering/wind_field.hpp"

#include <optional>

namespace wowee {
namespace rendering {

/// What the weather dev panel sets on the renderer, to put a particular
/// weather on screen and look at it: the weather and the wind to draw instead
/// of the realm's and the zone's, how fast it comes over, and which of its
/// parts are drawn.
struct WeatherControls {
    /// Drawn instead of the realm's weather while set.
    std::optional<WeatherTarget> weather;
    /// Blows instead of the zone's wind while set.
    std::optional<WindOverride> wind;
    float transitionSeconds = WeatherBlend::kDefaultTransitionSeconds;
    bool precipitation = true;
    bool splashes = true;
    bool lens = true;
    bool lightning = true;
};

}  // namespace rendering
}  // namespace wowee
