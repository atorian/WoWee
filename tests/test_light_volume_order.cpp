// The client's rule for laying Light.dbc volumes over each other
// (light_volume_order.hpp, WoW.exe 0x7f1360): what one weighs at a point, in
// what order they go down, and what each is left contributing once the ones
// laid over it have taken their part.
//
// The order is what decides which light a point mostly sees, so it has to be
// total: two volumes that are not the same volume always go down the same way
// round, whatever else is in the list. An unstable order once flipped the sky
// model from frame to frame while the player walked.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "rendering/light_volume_order.hpp"

using wowee::rendering::LightVolumePlace;
using wowee::rendering::lightVolumeLaidBefore;
using wowee::rendering::lightVolumeShares;
using wowee::rendering::lightVolumeWeight;

TEST_CASE("a volume weighs whole inside its inner radius and nothing past its outer one",
          "[lighting]") {
    CHECK(lightVolumeWeight(0.0f, 100.0f, 200.0f) == 1.0f);
    CHECK(lightVolumeWeight(100.0f, 100.0f, 200.0f) == 1.0f);
    CHECK(lightVolumeWeight(200.0f, 100.0f, 200.0f) == 0.0f);
    CHECK(lightVolumeWeight(350.0f, 100.0f, 200.0f) == 0.0f);
}

TEST_CASE("between its radii a volume's weight falls in a straight line", "[lighting]") {
    CHECK(lightVolumeWeight(125.0f, 100.0f, 200.0f) == Catch::Approx(0.75f));
    CHECK(lightVolumeWeight(150.0f, 100.0f, 200.0f) == Catch::Approx(0.5f));
    CHECK(lightVolumeWeight(175.0f, 100.0f, 200.0f) == Catch::Approx(0.25f));
}

TEST_CASE("the volume whose centre is farther goes down first, under the nearer",
          "[lighting]") {
    const LightVolumePlace far{.centre = {100.0f, 0.0f, 0.0f}, .distanceSq = 900.0f,
                               .innerRadius = 10.0f, .id = 2};
    const LightVolumePlace near{.centre = {0.0f, 0.0f, 0.0f}, .distanceSq = 100.0f,
                                .innerRadius = 500.0f, .id = 1};
    CHECK(lightVolumeLaidBefore(far, near));
    CHECK_FALSE(lightVolumeLaidBefore(near, far));
}

TEST_CASE("volumes on one centre go down larger first, so the tighter one lies on top",
          "[lighting]") {
    const LightVolumePlace large{.centre = {0.0f, 0.0f, 0.0f}, .distanceSq = 400.0f,
                                 .innerRadius = 300.0f, .id = 1};
    // A tenth of a yard away is the same centre; its distance is not what
    // decides.
    const LightVolumePlace small{.centre = {0.1f, 0.0f, 0.0f}, .distanceSq = 404.0f,
                                 .innerRadius = 50.0f, .id = 2};
    CHECK(lightVolumeLaidBefore(large, small));
    CHECK_FALSE(lightVolumeLaidBefore(small, large));
}

TEST_CASE("volumes that tie on everything still go down one way round", "[lighting]") {
    const LightVolumePlace a{.centre = {0.0f, 0.0f, 0.0f}, .distanceSq = 25.0f,
                             .innerRadius = 50.0f, .id = 7};
    const LightVolumePlace b{.centre = {0.0f, 0.0f, 0.0f}, .distanceSq = 25.0f,
                             .innerRadius = 50.0f, .id = 9};
    CHECK(lightVolumeLaidBefore(a, b));
    CHECK_FALSE(lightVolumeLaidBefore(b, a));
    CHECK_FALSE(lightVolumeLaidBefore(a, a));
}

TEST_CASE("the order of a set of volumes does not depend on the order they came in",
          "[lighting]") {
    std::vector<LightVolumePlace> places;
    for (uint32_t id = 1; id <= 12; ++id) {
        // Pairs on shared centres, and distances that repeat.
        const float x = static_cast<float>((id / 2) * 40);
        places.push_back({.centre = {x, 0.0f, 0.0f},
                          .distanceSq = static_cast<float>((id % 4) * 100),
                          .innerRadius = static_cast<float>(id * 10),
                          .id = id});
    }
    auto sorted = places;
    std::sort(sorted.begin(), sorted.end(), lightVolumeLaidBefore);

    std::mt19937 rng(12340);
    for (int round = 0; round < 50; ++round) {
        std::shuffle(places.begin(), places.end(), rng);
        auto again = places;
        std::sort(again.begin(), again.end(), lightVolumeLaidBefore);
        for (size_t i = 0; i < sorted.size(); ++i) CHECK(again[i].id == sorted[i].id);
    }
}

TEST_CASE("a volume laid over another takes its part from everything under it",
          "[lighting]") {
    // A zone's edge at half weight, and a hamlet inside it, also at half.
    const float weights[] = {0.5f, 0.5f};
    const auto shares = lightVolumeShares(weights);
    REQUIRE(shares.volumes.size() == 2);
    CHECK(shares.volumes[0] == Catch::Approx(0.25f));  // the zone, half of it covered
    CHECK(shares.volumes[1] == Catch::Approx(0.5f));   // the hamlet, on top
    CHECK(shares.global == Catch::Approx(0.25f));      // the map's own light, under both
}

TEST_CASE("a volume holding the point whole leaves nothing of what lies under it",
          "[lighting]") {
    const float weights[] = {0.7f, 1.0f};
    const auto shares = lightVolumeShares(weights);
    CHECK(shares.volumes[0] == 0.0f);
    CHECK(shares.volumes[1] == 1.0f);
    CHECK(shares.global == 0.0f);
}
