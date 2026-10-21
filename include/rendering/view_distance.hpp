#pragma once

namespace wowee::rendering {

// Shared by world renderers, saved settings and every UI/CVar entry point.
// Creature visibility and ground clutter have their own shorter ranges.
inline constexpr float kMinViewDistance = 400.0f;
inline constexpr float kMaxViewDistance = 10000.0f;
// Detailed ADT -> continent WDL overlap, selected for the long-view profile.
inline constexpr float kDistantTerrainStart = 3000.0f;
inline constexpr float kDistantTerrainEnd = 3500.0f;

} // namespace wowee::rendering
