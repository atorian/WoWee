#pragma once

#include "pipeline/weather_kind.hpp"
#include "rendering/wind_field.hpp"

#include <cstdint>
#include <optional>

namespace wowee {
namespace rendering {

/// What the game knows about the world that changes how it looks.
///
/// The renderer asks nothing of the game. Whoever runs it - the client, from
/// its realm connection and zone tracking, or an offline scene - hands this
/// over once a frame before update(), and a scene with no realm behind it
/// keeps the defaults: the local clock, clear weather, no zone.
struct WorldEnvironment {
    /// The world being drawn, for the Light.dbc lookup.
    uint32_t mapId = 0;
    /// The zone the player is in, already resolved from the area under them;
    /// 0 when nothing knows. Picks the zone's light and weather overrides.
    uint32_t zoneId = 0;
    /// The realm's time of day in hours; negative for none, when the sky
    /// follows the local clock.
    float gameTime = -1.0f;
    /// The realm's weather where the player stands (WeatherBlend draws it).
    pipeline::WeatherKind weatherKind = pipeline::WeatherKind::Clear;
    float weatherIntensity = 0.0f;
    /// The realm asked for it to arrive at once.
    bool weatherAbrupt = false;
    /// The player is a ghost: the world is drawn desaturated.
    bool ghost = false;

    /// What realmSeconds holds when no realm has said what time it is.
    static constexpr double kNoRealmTime = -1.0;
    /// The realm's calendar clock, seconds since 1970 (the game's RealmClock),
    /// or kNoRealmTime. Every client of a realm reads the same value at the
    /// same moment, which is what lets the wind be the same on all of them;
    /// without it the wind follows this machine's clock.
    double realmSeconds = kNoRealmTime;
    [[nodiscard]] bool hasRealmTime() const { return realmSeconds >= 0.0; }
    /// The subzone under the player, 0 when nothing knows. A wind row's
    /// "areas" match it, over its zone's row.
    uint32_t areaId = 0;
    /// A fixed wind, for an offline scene that wants to draw a particular one.
    /// The client leaves it empty and the zone's wind blows.
    std::optional<WindOverride> wind;
};

} // namespace rendering
} // namespace wowee
