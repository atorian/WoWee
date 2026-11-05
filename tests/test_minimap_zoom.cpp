// The Minimap frame's zoom, as the 3.3.5a client has it: six levels, one
// zoom for the terrain (minimapZoom) and one for a building's own tiles
// (minimapInsideZoom), SetZoom and GetZoom reaching the one for what the map
// shows, MINIMAP_UPDATE_ZOOM when that changes, and both kept in the CVars.
//
// Driven the way the client drives it: the interface calls the Minimap's
// methods from Lua, and the client tells the frame each frame whether the map
// draws a building (addons::setMinimapIndoors) and reads the two levels it
// draws at (addons::minimapZoomLevel).
#include <catch_amalgamated.hpp>

#include "client_harness.hpp"
#include "test_support.hpp"

#include "addons/lua_api_registrations.hpp"
#include "addons/lua_engine.hpp"
#include "addons/minimap_zoom.hpp"
#include "foundation/cvar_store.hpp"
#include "ui/widget_tree.hpp"

#include <string>

using wowee::test::ClientHarness;
namespace addons = wowee::addons;

namespace {

/// A character in the world whose interface has a Minimap, over a CVar store
/// of its own. The store is declared first so it outlives the interface.
struct MinimapWorld {
    wowee::core::CVarStore cvars;
    ClientHarness h;

    explicit MinimapWorld(const std::string& cvarFile = {}) : cvars(cvarFile) {
        cvars.load();
        addons::useCVarStore(cvars);
        h.enterWorld();
        h.runInInterface(R"lua(
            CreateFrame("Minimap", "Minimap", UIParent)
            ZOOM_UPDATES = 0
            local f = CreateFrame("Frame")
            f:RegisterEvent("MINIMAP_UPDATE_ZOOM")
            f:SetScript("OnEvent", function() ZOOM_UPDATES = ZOOM_UPDATES + 1 end)
        )lua");
    }

    /// What the client does each frame: tell the Minimap whether the map
    /// draws a building's own tiles.
    void mapShowsBuilding(bool building) {
        h.withInterface([&](addons::LuaEngine& engine) {
            auto* minimap = engine.widgets().findByName("Minimap");
            REQUIRE(minimap);
            addons::setMinimapIndoors(engine, *minimap, building);
        });
    }

    bool answers(const std::string& expression) { return h.interfaceAnswers(expression); }
};

} // namespace

TEST_CASE("the Minimap zooms in six steps, the last one level 5", "[minimap][interface]") {
    MinimapWorld w;
    w.mapShowsBuilding(false);

    // Minimap_ZoomInClick stops where GetZoom reaches GetZoomLevels() - 1.
    w.h.runInInterface("Minimap:SetZoom(0) for i = 1, 10 do Minimap:SetZoom(Minimap:GetZoom() + 1) end");
    CHECK(w.answers("Minimap:GetZoom() == 5"));
    CHECK(w.answers("Minimap:GetZoomLevels() - 1 == Minimap:GetZoom()"));
    CHECK(addons::minimapZoomLevel(false) == 5);
}

TEST_CASE("SetZoom drops the fraction and makes a level past the last the last",
          "[minimap][interface]") {
    MinimapWorld w;
    w.mapShowsBuilding(false);

    w.h.runInInterface("Minimap:SetZoom(2.9)");
    CHECK(w.answers("Minimap:GetZoom() == 2"));
    w.h.runInInterface("Minimap:SetZoom(40)");
    CHECK(w.answers("Minimap:GetZoom() == 5"));
    // Not a number: the client leaves the level where it was.
    w.h.runInInterface("Minimap:SetZoom(1) Minimap:SetZoom('closer')");
    CHECK(w.answers("Minimap:GetZoom() == 1"));
}

TEST_CASE("while the map draws a building, SetZoom and GetZoom reach the indoor zoom",
          "[minimap][interface]") {
    MinimapWorld w;
    w.mapShowsBuilding(false);
    w.h.runInInterface("Minimap:SetZoom(1)");

    w.mapShowsBuilding(true);
    // The indoor zoom has not been set: it stands where the client starts it.
    CHECK(w.answers("Minimap:GetZoom() == 3"));
    w.h.runInInterface("Minimap:SetZoom(5)");
    CHECK(w.answers("Minimap:GetZoom() == 5"));

    // The terrain's zoom is where it was left, and so is the building's.
    w.mapShowsBuilding(false);
    CHECK(w.answers("Minimap:GetZoom() == 1"));
    // And the map is drawn at those two.
    CHECK(addons::minimapZoomLevel(false) == 1);
    CHECK(addons::minimapZoomLevel(true) == 5);
}

TEST_CASE("the interface hears MINIMAP_UPDATE_ZOOM when first told and at each change of mode",
          "[minimap][interface]") {
    MinimapWorld w;

    w.mapShowsBuilding(false);
    CHECK(w.answers("ZOOM_UPDATES == 1"));
    w.mapShowsBuilding(false);
    CHECK(w.answers("ZOOM_UPDATES == 1"));
    w.mapShowsBuilding(true);
    CHECK(w.answers("ZOOM_UPDATES == 2"));
    w.mapShowsBuilding(true);
    CHECK(w.answers("ZOOM_UPDATES == 2"));
    w.mapShowsBuilding(false);
    CHECK(w.answers("ZOOM_UPDATES == 3"));
    // Zooming is the interface's own doing and it greys its buttons itself.
    w.h.runInInterface("Minimap:SetZoom(4)");
    CHECK(w.answers("ZOOM_UPDATES == 3"));
}

TEST_CASE("both zooms are where the player left them at the next start", "[minimap][interface]") {
    const auto dir = wowee::test::scratchDir("wowee_minimap_zoom");
    const std::string file = (dir / "cvars.cfg").string();
    {
        MinimapWorld w(file);
        w.mapShowsBuilding(false);
        w.h.runInInterface("Minimap:SetZoom(2)");
        w.mapShowsBuilding(true);
        w.h.runInInterface("Minimap:SetZoom(4)");
    }
    // In the file, under the client's names.
    wowee::core::CVarStore written(file);
    written.load();
    CHECK(written.get("minimapZoom", "") == "2");
    CHECK(written.get("minimapInsideZoom", "") == "4");

    MinimapWorld next(file);
    next.mapShowsBuilding(true);
    CHECK(next.answers("Minimap:GetZoom() == 4"));
    next.mapShowsBuilding(false);
    CHECK(next.answers("Minimap:GetZoom() == 2"));
}

TEST_CASE("a zoom sent to another frame does not move the map", "[minimap][interface]") {
    MinimapWorld w;
    w.mapShowsBuilding(false);
    w.h.runInInterface("Minimap:SetZoom(2) CreateFrame('Minimap', 'OtherMap'):SetZoom(5)");
    CHECK(w.answers("Minimap:GetZoom() == 2"));
    CHECK(addons::minimapZoomLevel(false) == 2);
}

TEST_CASE("a stored zoom that is not a level is drawn at the nearest one", "[minimap][interface]") {
    MinimapWorld w;
    w.mapShowsBuilding(false);
    w.h.runInInterface("SetCVar('minimapZoom', '9') SetCVar('minimapInsideZoom', '-2')");
    CHECK(addons::minimapZoomLevel(false) == 5);
    CHECK(addons::minimapZoomLevel(true) == 0);
    CHECK(w.answers("Minimap:GetZoom() == 5"));
}
