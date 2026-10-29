// Which WMO batches each layer of the scene pass draws. The opaque layer goes
// ahead of the sky and the blended one after it, so a batch in neither is
// never drawn, and a window put in the opaque layer is painted over by the
// sky behind it.
#include <catch_amalgamated.hpp>
#include "rendering/wmo_draw_layer.hpp"

using wowee::rendering::WmoDrawLayer;
using wowee::rendering::wmoBatchInLayer;

TEST_CASE("A wall is drawn ahead of the sky and not again after it", "[wmo][layer]") {
    REQUIRE(wmoBatchInLayer(false, WmoDrawLayer::Opaque));
    REQUIRE_FALSE(wmoBatchInLayer(false, WmoDrawLayer::Blended));
}

TEST_CASE("A window is drawn after the sky and not ahead of it", "[wmo][layer]") {
    REQUIRE(wmoBatchInLayer(true, WmoDrawLayer::Blended));
    REQUIRE_FALSE(wmoBatchInLayer(true, WmoDrawLayer::Opaque));
}

TEST_CASE("A pass drawn whole draws walls and windows alike", "[wmo][layer]") {
    REQUIRE(wmoBatchInLayer(false, WmoDrawLayer::All));
    REQUIRE(wmoBatchInLayer(true, WmoDrawLayer::All));
}
