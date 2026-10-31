// Where the flight map draws a node: TaxiNodePosition, as FrameXML asks it.
//
// The flight map places a node by the chain the world map places the player
// by - server position, canonical, render, UV against the continent rectangle -
// and then counts up from the bottom, because taxiframe.lua anchors its markers
// to BOTTOMLEFT where the world map anchors to TOPLEFT. Getting any step wrong
// does not fail: it draws every marker somewhere plausible and wrong.
//
// It has been wrong twice. Feeding TaxiNodes.dbc positions straight into
// canonicalToRender transposed every node, and answering the world map's
// top-down fraction mirrored the map vertically - Rut'theran was drawn over
// Tanaris.
//
// The expected fractions follow from WorldMapArea.dbc's own convention: X runs
// north and Y runs west, LocLeft/LocRight bound Y (west, east) and
// LocTop/LocBottom bound X (north, south). So a node's fraction across is
// (LocLeft - Y) / (LocLeft - LocRight) and its fraction up from the bottom is
// (X - LocBottom) / (LocTop - LocBottom).
#include <catch_amalgamated.hpp>

#include "client_harness.hpp"
#include "game_harness.hpp"

#include "game/opcode_table.hpp"

#include <string>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::ClientHarness;

namespace {

/// SMSG_SHOWTAXINODES: window, the flight master, the nearest node, then the
/// twelve words of the known-node mask (node n is bit n - 1).
Body showTaxiNodes(uint32_t nearest, uint32_t mask0) {
    Body b;
    b.u32(1).u64(0xF130000000003003ull).u32(nearest).u32(mask0);
    for (int i = 1; i < 12; ++i) b.u32(0);
    return b;
}

/// A character in the world on map 0, whose flight master knows two nodes.
///
/// Kalimdor-shaped continent rectangle: 32000 yards west to east, 20000 south
/// to north, centred on the origin.
struct FlightMap {
    ClientHarness h;

    FlightMap() {
        auto& server = h.server();
        // ID, map, X (north), Y (west), Z, name.
        server.dbc("TaxiNodes.dbc", 24,
                   {{{0, 2}, {1, 0}, {2, 5000.0f}, {3, 8000.0f}, {4, 0.0f}, {5, "Northwest"}},
                    {{0, 4}, {1, 0}, {2, -9000.0f}, {3, -12000.0f}, {4, 0.0f}, {5, "Southeast"}}});
        server.dbc("TaxiPath.dbc", 4, {});
        // ID, map, area (0: the continent's own row), name, then LocLeft,
        // LocRight, LocTop, LocBottom.
        server.dbc("WorldMapArea.dbc", 11,
                   {{{0, 13}, {1, 0}, {2, 0}, {3, "Kalimdor"},
                     {4, 16000.0f}, {5, -16000.0f}, {6, 10000.0f}, {7, -10000.0f}}});
        h.enterWorld();
        server.receive(Opcode::SMSG_SHOWTAXINODES, showTaxiNodes(2, 0b1010));
        server.pump();
    }

    /// Whether TaxiNodePosition(index) answers (x, y), to four places.
    bool nodeAt(int index, double x, double y) {
        return h.interfaceAnswers(
            "(function() local x, y = TaxiNodePosition(" + std::to_string(index) + ") "
            "return x ~= nil and math.abs(x - " + std::to_string(x) + ") < 1e-4 "
            "and math.abs(y - " + std::to_string(y) + ") < 1e-4 end)()");
    }
};

}  // namespace

TEST_CASE("a node in the northwest is drawn left of centre and above it", "[taxi][map][interface]") {
    FlightMap map;
    // Across: (16000 - 8000) / 32000. Up: (5000 + 10000) / 20000.
    CHECK(map.nodeAt(1, 0.25, 0.75));
}

TEST_CASE("a node in the southeast is drawn right of centre and below it", "[taxi][map][interface]") {
    FlightMap map;
    // Across: (16000 + 12000) / 32000. Up: (-9000 + 10000) / 20000.
    CHECK(map.nodeAt(2, 0.875, 0.05));
}

TEST_CASE("a node past the last one has no position", "[taxi][map][interface]") {
    FlightMap map;
    CHECK(map.h.interfaceAnswers("TaxiNodePosition(3) == nil"));
}
