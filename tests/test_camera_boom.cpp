// The third-person camera's distance against the 3.3.5a client's (CGCamera,
// WoW.exe 12340 0x605d60): pulled in at once, short of what it meets by a
// ninth of a yard, and eased back out over two seconds once clear.
#include <catch_amalgamated.hpp>

#include "rendering/camera_boom.hpp"

using namespace wowee::rendering;

TEST_CASE("Nothing in the way: the camera sits at the chosen distance", "[camera][boom]") {
    float current = 10.0f;
    CameraBoomEase ease;
    const float reach = cameraBoomReach(current, 10.0f);
    CHECK(cameraBoomPlace(current, ease, reach, reach) == Catch::Approx(10.0f));
    CHECK_FALSE(ease.active);
}

TEST_CASE("A wall pulls the camera in at once and the chosen distance stays", "[camera][boom]") {
    float current = 10.0f;
    CameraBoomEase ease;
    const float wanted = 10.0f;
    const float placed = cameraBoomPlace(current, ease, 4.0f, cameraBoomReach(current, wanted));
    CHECK(placed == Catch::Approx(4.0f - kCameraBoomPad));
    CHECK(current == Catch::Approx(4.0f));
    CHECK(ease.active);
    // Held there while the wall stays, the ease starting over each frame.
    cameraBoomApproach(current, ease, wanted, 0.5f);
    cameraBoomPlace(current, ease, 4.0f, cameraBoomReach(current, wanted));
    CHECK(current == Catch::Approx(4.0f));
    CHECK(ease.elapsed == 0.0f);
}

TEST_CASE("Clear again, the camera eases back out over two seconds", "[camera][boom]") {
    float current = 4.0f;
    CameraBoomEase ease{.active = true, .from = 4.0f, .elapsed = 0.0f};
    const float wanted = 10.0f;
    cameraBoomApproach(current, ease, wanted, 1.0f);
    // Halfway through a cosine ease is halfway there.
    CHECK(current == Catch::Approx(7.0f));
    float reach = cameraBoomReach(current, wanted);
    CHECK(cameraBoomPlace(current, ease, reach, reach) == Catch::Approx(7.0f));
    cameraBoomApproach(current, ease, wanted, 1.0f);
    CHECK(current == Catch::Approx(10.0f));
    CHECK_FALSE(ease.active);
}

TEST_CASE("The wheel moves the camera at its own speed", "[camera][boom]") {
    float current = 10.0f;
    CameraBoomEase ease;
    cameraBoomApproach(current, ease, 20.0f, 0.5f);
    CHECK(current == Catch::Approx(10.0f + kCameraBoomWheelSpeed * 0.5f));
    cameraBoomApproach(current, ease, 5.0f, 10.0f);
    CHECK(current == Catch::Approx(5.0f));
}

TEST_CASE("The player fades as the camera comes close", "[camera][boom]") {
    const float nearPlane = 0.2f;
    CHECK(cameraCloseFade(10.0f, nearPlane) == 1.0f);
    CHECK(cameraCloseFade(nearPlane + kCameraCloseFadeDistance, nearPlane) == 1.0f);
    CHECK(cameraCloseFade(nearPlane, nearPlane) == 0.0f);
    const float half = cameraCloseFade(nearPlane + 1.0f / 360.0f + (kCameraCloseFadeDistance - 1.0f / 360.0f) * 0.5f,
                                       nearPlane);
    CHECK(half == Catch::Approx(0.5f));
}

TEST_CASE("A wall right behind the character puts the camera in first person", "[camera][boom]") {
    float current = 10.0f;
    CameraBoomEase ease;
    CHECK(cameraBoomPlace(current, ease, 0.05f, cameraBoomReach(current, 10.0f)) == 0.0f);
}
