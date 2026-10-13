#pragma once

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
    bool raining = false;
    /// The realm's weather: 0 for none, when the zone's own weather plays,
    /// 1 rain, 2 snow, 3 storm.
    uint32_t weatherType = 0;
    float weatherIntensity = 0.0f;
    /// The player is a ghost: the world is drawn desaturated.
    bool ghost = false;
    /// The player's facing in canonical yaw (north 0, east +pi/2), for the
    /// minimap arrow when no follow camera says which way they face.
    std::optional<float> playerOrientation;
};

} // namespace rendering
} // namespace wowee
