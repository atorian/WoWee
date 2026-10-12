// Whether the water the depth query found is the water the camera is in.
// The query reaches 400 units up so deep ocean counts, which also finds a
// lake above a dry cave.
#include <catch_amalgamated.hpp>

#include "rendering/camera_water_depth.hpp"

using wowee::rendering::cameraWaterDepth;

TEST_CASE("a camera in a lake is under it by the distance to the surface", "[water]") {
    // Lake bed at 90, surface at 100, camera at 95.
    const auto depth = cameraWaterDepth(95.0f, 100.0f, 90.0f, false);
    REQUIRE(depth.has_value());
    CHECK(*depth == 5.0f);
}

TEST_CASE("deep ocean stays under water however far down", "[water]") {
    // Seabed 600 down, camera 350 under the surface.
    const auto depth = cameraWaterDepth(-350.0f, 0.0f, -600.0f, false);
    REQUIRE(depth.has_value());
    CHECK(*depth == 350.0f);
}

TEST_CASE("a dry cave under a lake is not under water", "[water]") {
    // Lake bed at 90, surface at 100; the cave camera is 30 below the bed.
    CHECK_FALSE(cameraWaterDepth(60.0f, 100.0f, 90.0f, false).has_value());
}

TEST_CASE("a camera brushing the lake bed is still in the lake", "[water]") {
    const auto depth = cameraWaterDepth(89.7f, 100.0f, 90.0f, false);
    REQUIRE(depth.has_value());
    CHECK(*depth == Catch::Approx(10.3f));
}

TEST_CASE("over a terrain hole or an unloaded tile the water decides", "[water]") {
    CHECK(cameraWaterDepth(60.0f, 100.0f, 90.0f, true).has_value());
    CHECK(cameraWaterDepth(60.0f, 100.0f, std::nullopt, false).has_value());
    CHECK_FALSE(cameraWaterDepth(60.0f, std::nullopt, 90.0f, false).has_value());
}
