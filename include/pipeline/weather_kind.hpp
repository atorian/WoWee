#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wowee {
namespace pipeline {

/// What kind of weather a place has, whatever numbering the realm speaks.
///
/// The game decodes the realm's SMSG_WEATHER into it (WeatherHandler) and the
/// renderer draws it (WeatherBlend); the wind table names what each kind does
/// to the wind. Shared here because neither of those may include the other.
enum class WeatherKind : uint8_t {
    Clear,
    /// Mist and nothing falling - WotLK's WEATHER_STATE_FOG, which the realm
    /// uses in a few encounters.
    Fog,
    Rain,
    Snow,
    Sandstorm,
    /// Heavy rain under lightning (WEATHER_STATE_THUNDERS).
    Thunderstorm,
    /// Rain and snow falling dark, as over a burning land
    /// (WEATHER_STATE_BLACKRAIN, _BLACKSNOW).
    BlackRain,
    BlackSnow,
};

inline constexpr std::array<WeatherKind, 8> kWeatherKinds = {
    WeatherKind::Clear,     WeatherKind::Fog,          WeatherKind::Rain,      WeatherKind::Snow,
    WeatherKind::Sandstorm, WeatherKind::Thunderstorm, WeatherKind::BlackRain, WeatherKind::BlackSnow,
};

/// The kind's name in scene files, the dev panel and the log: "rain".
[[nodiscard]] constexpr std::string_view weatherKindName(WeatherKind kind) {
    switch (kind) {
        case WeatherKind::Clear: return "clear";
        case WeatherKind::Fog: return "fog";
        case WeatherKind::Rain: return "rain";
        case WeatherKind::Snow: return "snow";
        case WeatherKind::Sandstorm: return "sandstorm";
        case WeatherKind::Thunderstorm: return "thunderstorm";
        case WeatherKind::BlackRain: return "blackrain";
        case WeatherKind::BlackSnow: return "blacksnow";
    }
    return "clear";
}

/// The kind a name stands for, or empty for a name that is none of them.
[[nodiscard]] constexpr std::optional<WeatherKind> weatherKindFromName(std::string_view name) {
    for (const WeatherKind kind : kWeatherKinds) {
        if (weatherKindName(kind) == name) return kind;
    }
    return std::nullopt;
}

}  // namespace pipeline
}  // namespace wowee
