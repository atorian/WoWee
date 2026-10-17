// Which light a WMO group is drawn with: the sun and the zone's ambient, or
// the light baked into its vertex colours.
//
// These cases lived beside the glass classification, which went with the glass
// path; the lighting they check did not.
#include <catch_amalgamated.hpp>

#include "rendering/wmo_lighting.hpp"

TEST_CASE("Orgrimmar streets can use exterior lighting inside portal groups", "[wmo-light]") {
    using namespace wowee::rendering;
    // Real MOGP flags: bazzaar/AssassinCanyon, BigCanyon, throne rooms.
    CHECK(wmoUsesExteriorLighting(0x2841, 0));
    CHECK_FALSE(wmoUsesBakedInteriorLighting(0x2841, 0));
    CHECK(wmoReceivesExteriorShadows(0x2841, true));
    CHECK(wmoUsesBakedInteriorLighting(0x2905, 0));
    CHECK(wmoReceivesExteriorShadows(0x2905, false));
    CHECK(wmoUsesBakedInteriorLighting(0x2805, 0));
    CHECK_FALSE(wmoReceivesExteriorShadows(0x2805, false));
    CHECK(wmoUsesExteriorLighting(0x809, 0));
    CHECK_FALSE(wmoUsesBakedInteriorLighting(0x2805, 0x8));
}
