// What lights the frame while the camera is inside a WMO interior group:
// which groups count as inside, where the building's ambient comes from in the
// file, and what that ambient replaces.

#include <catch_amalgamated.hpp>

#include "pipeline/wmo_loader.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/wmo_group_containment.hpp"

#include <cstring>
#include <vector>

using wowee::rendering::LightingParams;
using wowee::rendering::applyInteriorLight;
using wowee::rendering::groupContainsPoint;

namespace {

struct Group {
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    uint32_t groupFlags;
};

void put32(std::vector<uint8_t>& out, uint32_t v) {
    const size_t at = out.size();
    out.resize(at + 4);
    std::memcpy(&out[at], &v, 4);
}

// A root WMO holding only MVER and MOHD, with MOHD's ambient written as the
// four bytes the file stores, in file order.
std::vector<uint8_t> rootWithAmbientBytes(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    std::vector<uint8_t> file;
    put32(file, 0x4D564552);  // MVER
    put32(file, 4);
    put32(file, 17);
    put32(file, 0x4D4F4844);  // MOHD
    put32(file, 64);
    for (int i = 0; i < 7; ++i) put32(file, 0);  // the seven counts
    file.insert(file.end(), {b0, b1, b2, b3});
    while (file.size() < 8 + 12 + 64) file.push_back(0);  // wmoID, box, flags
    return file;
}

}  // namespace

TEST_CASE("an exterior group of a WMO does not count as inside it for lighting",
          "[interior_light]") {
    // Stormwind's streets: inside the city WMO's box, in a group flagged
    // exterior (0x8) and not interior.
    const Group street{.boundingBoxMin = {0, 0, 0}, .boundingBoxMax = {100, 100, 30},
                       .groupFlags = 0x8};
    const glm::vec3 onTheStreet(50, 50, 5);

    REQUIRE_FALSE(groupContainsPoint(street, onTheStreet, /*interiorOnly=*/true));
    // Still inside the WMO for everything that asks about any group.
    REQUIRE(groupContainsPoint(street, onTheStreet, /*interiorOnly=*/false));
}

TEST_CASE("an interior group counts as inside only within its own box", "[interior_light]") {
    const Group hall{.boundingBoxMin = {-10, -10, 0}, .boundingBoxMax = {10, 10, 8},
                     .groupFlags = 0x2000};

    REQUIRE(groupContainsPoint(hall, glm::vec3(0, 0, 2), /*interiorOnly=*/true));
    REQUIRE_FALSE(groupContainsPoint(hall, glm::vec3(0, 0, 9), /*interiorOnly=*/true));
    REQUIRE_FALSE(groupContainsPoint(hall, glm::vec3(11, 0, 2), /*interiorOnly=*/true));
}

TEST_CASE("MOHD ambient is read as blue, green, red, alpha bytes", "[interior_light]") {
    // CImVector: the file stores B, G, R, A.
    const auto model = wowee::pipeline::WMOLoader::load(
        rootWithAmbientBytes(/*B*/ 0xFF, /*G*/ 0x00, /*R*/ 0x33, /*A*/ 0xFF));

    REQUIRE(model.ambientColor.r == Catch::Approx(0.2f));
    REQUIRE(model.ambientColor.g == Catch::Approx(0.0f));
    REQUIRE(model.ambientColor.b == Catch::Approx(1.0f));
}

TEST_CASE("indoors the frame is lit by the WMO's ambient and not by the sun",
          "[interior_light]") {
    LightingParams params;
    params.ambientColor = glm::vec3(0.4f, 0.4f, 0.5f);   // the zone's LightParams
    params.diffuseColor = glm::vec3(1.0f, 0.95f, 0.8f);  // the sun

    applyInteriorLight(params, glm::vec3(0.25f, 0.2f, 0.15f));

    REQUIRE(params.ambientColor == glm::vec3(0.25f, 0.2f, 0.15f));
    REQUIRE(params.diffuseColor == glm::vec3(0.0f));
}

TEST_CASE("indoors the fog and sky stay the outdoor ones seen through the door",
          "[interior_light]") {
    LightingParams params;
    params.fogColor = glm::vec3(0.1f, 0.2f, 0.3f);
    params.fogStart = 120.0f;
    params.fogEnd = 640.0f;
    params.skyTopColor = glm::vec3(0.02f, 0.03f, 0.08f);  // night
    params.skyMiddleColor = glm::vec3(0.05f, 0.06f, 0.1f);
    params.directionalDir = glm::vec3(0.0f, 0.6f, -0.8f);

    applyInteriorLight(params, glm::vec3(0.25f, 0.2f, 0.15f));

    REQUIRE(params.fogColor == glm::vec3(0.1f, 0.2f, 0.3f));
    REQUIRE(params.fogStart == 120.0f);
    REQUIRE(params.fogEnd == 640.0f);
    REQUIRE(params.skyTopColor == glm::vec3(0.02f, 0.03f, 0.08f));
    REQUIRE(params.skyMiddleColor == glm::vec3(0.05f, 0.06f, 0.1f));
    REQUIRE(params.directionalDir == glm::vec3(0.0f, 0.6f, -0.8f));
}

TEST_CASE("a WMO with an all-zero ambient keeps the zone's ambient indoors",
          "[interior_light]") {
    LightingParams params;
    params.ambientColor = glm::vec3(0.4f, 0.4f, 0.5f);
    params.diffuseColor = glm::vec3(1.0f, 0.95f, 0.8f);

    applyInteriorLight(params, glm::vec3(0.0f));

    REQUIRE(params.ambientColor == glm::vec3(0.4f, 0.4f, 0.5f));
    REQUIRE(params.diffuseColor == glm::vec3(0.0f));
}
