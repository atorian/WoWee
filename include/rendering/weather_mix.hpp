#pragma once

#include "pipeline/weather_kind.hpp"

#include <algorithm>

namespace wowee {
namespace rendering {

/// The weather a renderer is asked to draw: the realm's, or a scene's or the
/// dev panel's own.
struct WeatherTarget {
    pipeline::WeatherKind kind = pipeline::WeatherKind::Clear;
    float intensity = 0.0f;  ///< 0-1
    /// Arrives at once rather than coming over.
    bool abrupt = false;

    bool operator==(const WeatherTarget&) const = default;
};

/// What the weather is made of as it is drawn this frame, each part 0-1.
/// While one weather turns into another both are in it, the one going and the
/// one coming.
struct WeatherMix {
    float rain = 0.0f;     ///< rain falling: rain, thunderstorms, black rain
    float snow = 0.0f;     ///< snow falling, black snow too
    float sand = 0.0f;     ///< sand blowing
    float mist = 0.0f;     ///< the fog weather's own mist, nothing falling
    float thunder = 0.0f;  ///< lightning over the rain
    float dark = 0.0f;     ///< how black what falls is

    /// How heavy the weather is as the sky and the light take it - the storm
    /// light row, the sun's disc and glare fading out: its heaviest part.
    [[nodiscard]] float strength() const { return std::max({rain, snow, sand, mist}); }
    /// Anything falling or blowing at all.
    [[nodiscard]] bool any() const { return strength() > 0.0f; }

    bool operator==(const WeatherMix&) const = default;
};

/// The parts a weather is made of when it has fully arrived.
[[nodiscard]] WeatherMix weatherMixOf(const WeatherTarget& target);

/// The weather drawn this frame, coming toward the weather asked for.
///
/// Every part moves toward what the asked-for weather is made of by at most
/// dt / transitionSeconds a frame: a shower takes the transition time to
/// arrive from nothing, rain turning to snow fades out while the snow comes
/// in. Weather that asks to arrive at once does, and so does the first weather
/// a renderer is given - a scene or a login in the rain starts in the rain.
class WeatherBlend {
public:
    static constexpr float kDefaultTransitionSeconds = 15.0f;

    void update(float dt, const WeatherTarget& target);

    [[nodiscard]] const WeatherMix& mix() const { return mix_; }
    [[nodiscard]] const WeatherTarget& target() const { return target_; }

    /// How long the weather takes to come over from nothing, in seconds.
    void setTransitionSeconds(float seconds) { transitionSeconds_ = std::max(seconds, 0.0f); }
    [[nodiscard]] float transitionSeconds() const { return transitionSeconds_; }

private:
    WeatherMix mix_;
    WeatherTarget target_;
    float transitionSeconds_ = kDefaultTransitionSeconds;
    bool started_ = false;
};

}  // namespace rendering
}  // namespace wowee
