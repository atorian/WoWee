#include "addons/minimap_zoom.hpp"

#include "addons/lua_api_registrations.hpp"
#include "addons/lua_engine.hpp"
#include "foundation/logger.hpp"
#include "ui/widget_tree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace wowee::addons {

namespace {

const char* zoomCVar(bool indoors) { return indoors ? "minimapInsideZoom" : "minimapZoom"; }

bool indoorsOf(const ui::Widget& minimap) { return minimap.minimapIndoors.value_or(false); }

/// The level a stored value stands for. As the client reads a CVar's number:
/// the digits it starts with. Past either end, the end - the client would
/// index past its table of radii.
int levelOf(const std::string& text) {
    return std::clamp(std::atoi(text.c_str()), 0, kMinimapZoomLevels - 1);
}

} // namespace

int minimapZoomLevel(bool indoors) {
    return levelOf(storedCVarValue(zoomCVar(indoors), std::to_string(kMinimapDefaultZoom)));
}

void noteMinimapZoomCVar(const std::string& name, const std::string& value) {
    const int level = levelOf(value);
    if (std::to_string(level) == value) return;
    LOG_WARNING("Minimap: ", name, " is \"", value, "\", not a zoom level 0 to ",
                kMinimapZoomLevels - 1, "; the map is drawn at level ", level);
}

int minimapZoom(const ui::Widget& minimap) {
    return minimapZoomLevel(indoorsOf(minimap));
}

void setMinimapZoom(const ui::Widget& minimap, double level) {
    // The client compares the level unsigned, so it makes a negative one the
    // last as well. Here it is the first: Button:Click runs OnClick on a
    // disabled button, so the zoom-out binding pressed at level 0 asks for -1,
    // and the map would jump to its closest zoom.
    const int last = kMinimapZoomLevels - 1;
    const int kept = !(level >= 0.0) ? 0 : level >= last ? last : static_cast<int>(std::trunc(level));
    const char* cvar = zoomCVar(indoorsOf(minimap));
    // Kept as the client keeps it, in the CVar, which the store writes to
    // disk as it changes: the zoom is where the player left it at the next
    // start.
    setStoredCVar(cvar, std::to_string(kept));
}

void setMinimapIndoors(LuaEngine& engine, ui::Widget& minimap, bool indoors) {
    if (minimap.minimapIndoors == indoors) return;
    minimap.minimapIndoors = indoors;
    engine.fireEvent("MINIMAP_UPDATE_ZOOM", {});
}

} // namespace wowee::addons
