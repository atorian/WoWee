#pragma once

// The Minimap frame's zoom, as the 3.3.5a client keeps it (wow.exe build
// 12340, read from its code): two levels, one for the map drawn from the
// terrain (the minimapZoom CVar) and one for a building's own tiles
// (minimapInsideZoom), and SetZoom and GetZoom reach the one for what the map
// shows now.

#include <string>

namespace wowee::ui { struct Widget; }

namespace wowee::addons {

class LuaEngine;

/// Steps in either zoom. GetZoomLevels answers a constant 6 (0x75b710).
inline constexpr int kMinimapZoomLevels = 6;

/// The level both zooms stand at until set, as the client registers their
/// CVars (0x4ab570).
inline constexpr int kMinimapDefaultZoom = 3;

/// The level a zoom stands at: minimapInsideZoom when `indoors`, minimapZoom
/// otherwise.
[[nodiscard]] int minimapZoomLevel(bool indoors);

/// Say so in the log when a value put in either zoom's CVar - by SetCVar, or
/// in the file read at the start - is not a level, and which level the map is
/// drawn at instead. `name` is folded, as the CVar store keys it.
void noteMinimapZoomCVar(const std::string& name, const std::string& value);

/// Minimap:GetZoom - the level of the zoom the frame was last told it is in;
/// the outdoor one before it was told anything.
[[nodiscard]] int minimapZoom(const ui::Widget& minimap);

/// Minimap:SetZoom - set and keep that zoom's level. As the client takes it
/// (0x75b690): the fraction dropped, a level past the last made the last.
void setMinimapZoom(const ui::Widget& minimap, double level);

/// Tell the Minimap whether the map shows a building's own tiles, which
/// decides the zoom SetZoom and GetZoom reach. The first time, and whenever
/// that changes, the interface hears MINIMAP_UPDATE_ZOOM, as the client fires
/// it when it reads the two levels (0x75e327) and when the map changes mode
/// (0x75d7dd) - it is what greys the zoom button that can go no further.
void setMinimapIndoors(LuaEngine& engine, ui::Widget& minimap, bool indoors);

} // namespace wowee::addons
