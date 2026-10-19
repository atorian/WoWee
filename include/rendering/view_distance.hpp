#pragma once

namespace wowee::rendering {

// Shared by world renderers, saved settings and every UI/CVar entry point.
// Creature visibility and ground clutter have their own shorter ranges.
inline constexpr float kMinViewDistance = 400.0f;
inline constexpr float kMaxViewDistance = 4000.0f;

} // namespace wowee::rendering
