#include <catch_amalgamated.hpp>
#include "rendering/local_lights.hpp"

using namespace wowee::rendering;

namespace {
LocalLight at(float x, float y, float z = 0.0f) { return {{x, y, z}, {1.0f, 1.0f, 1.0f}}; }
}  // namespace

TEST_CASE("a receiver takes the nearest lights, nearest first", "[lights]") {
    std::vector<LocalLight> lights;
    for (float x : {9.f, 1.f, 5.f, 3.f, 7.f, 2.f, 8.f, 4.f, 6.f, 10.f}) lights.push_back(at(x, 0));
    const LocalLightSet p = pickLocalLights(lights, glm::vec3(0.0f), 0.0f);
    // Eight of the ten, by distance: x = 1, 2, 3 ... 8.
    const uint32_t expected[] = {1, 5, 3, 7, 2, 8, 4, 6};
    for (int k = 0; k < kLocalLightsPerReceiver; ++k) {
        INFO("slot " << k);
        CHECK(localLightSlot(p, k) == expected[k]);
    }
}

TEST_CASE("fewer lights leave the rest of the slots empty", "[lights]") {
    CHECK(pickLocalLights({}, glm::vec3(0.0f), 0.0f) == kNoLocalLights);
    const LocalLightSet one = pickLocalLights({at(1, 1)}, glm::vec3(0.0f), 0.0f);
    CHECK(localLightSlot(one, 0) == 0);
    for (int k = 1; k < kLocalLightsPerReceiver; ++k) CHECK(localLightSlot(one, k) == kNoLocalLightIndex);
    const LocalLightSet capped = pickLocalLights({at(1, 0), at(2, 0), at(3, 0)}, glm::vec3(0.0f), 0.0f, 2);
    CHECK(localLightSlot(capped, 1) == 1);
    CHECK(localLightSlot(capped, 2) == kNoLocalLightIndex);
}

TEST_CASE("indices past 255 are addressable", "[lights]") {
    std::vector<LocalLight> lights(400, at(1000, 1000));
    lights[300] = at(1, 0);
    CHECK(localLightSlot(pickLocalLights(lights, glm::vec3(0.0f), 0.0f), 0) == 300);
}

TEST_CASE("only the grid cells around a receiver are searched", "[lights]") {
    // A point receiver at the middle of a 20-yard cell sees that cell and its
    // neighbours toward the nearer side: floor(c/20 - 0.5) .. floor(c/20 + 0.5).
    CHECK(localLightCellRange(30.0f, 0.0f) == std::array<int, 2>{1, 2});
    CHECK(localLightCellRange(25.0f, 0.0f) == std::array<int, 2>{0, 1});
    // A light 41 yards away along x is outside that window, whatever its brightness.
    CHECK(pickLocalLights({at(71, 30)}, glm::vec3(30.0f, 30.0f, 0.0f), 0.0f) == kNoLocalLights);
    // A big receiver reaches further.
    const LocalLightSet big = pickLocalLights({at(71, 30)}, glm::vec3(30.0f, 30.0f, 0.0f), 40.0f);
    CHECK(localLightSlot(big, 0) == 0);
}

TEST_CASE("a light falls off by the fixed client attenuation", "[lights]") {
    CHECK(localLightAttenuation(10.0f) == Catch::Approx(1.0f / (7.0f + 3.0f)));
    CHECK(localLightAttenuation(20.0f) == Catch::Approx(1.0f / (14.0f + 12.0f)));
}
