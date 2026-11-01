// The LOGON_CHALLENGE timezone field.
//
// Wow.exe 12340 sends -Bias from GetTimeZoneInformation (0x86D450): minutes
// east of UTC in standard time, daylight saving left out. The client sent 0.
// Daylight saving only ever moves a clock east, so the standard offset is the
// smaller of the January and July offsets on either hemisphere.
#include <catch_amalgamated.hpp>

#include "auth/client_timezone.hpp"

#include <cstdint>

using wowee::auth::standardBiasMinutes;

TEST_CASE("The timezone field is the standard offset in minutes east of UTC",
          "[auth][timezone]") {
    SECTION("Central Europe: +1:00, +2:00 in summer") {
        CHECK(standardBiasMinutes(3600, 7200) == 60u);
    }
    SECTION("New York: -5:00, -4:00 in summer - a negative bias on the wire") {
        CHECK(standardBiasMinutes(-18000, -14400) == static_cast<uint32_t>(-300));
    }
    SECTION("Sydney: summer falls in January") {
        CHECK(standardBiasMinutes(39600, 36000) == 600u);
    }
    SECTION("India: +5:30 all year") {
        CHECK(standardBiasMinutes(19800, 19800) == 330u);
    }
    SECTION("UTC") {
        CHECK(standardBiasMinutes(0, 0) == 0u);
    }
}
