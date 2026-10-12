// What lights a model standing inside a WMO interior group: which groups count
// as inside, which doodads the file puts inside them, where the building's
// ambient comes from, how a unit is lit by the floor under it, and what that
// light replaces in the shaders.

#include <catch_amalgamated.hpp>

#include "pipeline/wmo_loader.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/wmo_group_containment.hpp"

#include <glm/glm.hpp>

#include <cstring>
#include <vector>

namespace interior_light {
using glm::vec3;
using glm::vec4;
#include "../assets/shaders/interior_light.glsli"
}

using wowee::rendering::colorAtPoint;
using wowee::rendering::doodadInteriorLight;
using wowee::rendering::groupContainsPoint;
using wowee::rendering::InteriorLight;
using wowee::rendering::InteriorLightGPU;
using wowee::rendering::packInteriorLight;
using wowee::rendering::unitInteriorLight;

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

void put16(std::vector<uint8_t>& out, uint16_t v) {
    const size_t at = out.size();
    out.resize(at + 2);
    std::memcpy(&out[at], &v, 2);
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

// A group file holding MVER and a MOGP whose only sub-chunk is a MODR listing
// `refs`.
std::vector<uint8_t> groupWithDoodadRefs(uint32_t flags, const std::vector<uint16_t>& refs) {
    std::vector<uint8_t> modr;
    put32(modr, 0x4D4F4452);  // MODR
    put32(modr, static_cast<uint32_t>(refs.size() * 2));
    for (uint16_t ref : refs) put16(modr, ref);

    std::vector<uint8_t> file;
    put32(file, 0x4D564552);  // MVER
    put32(file, 4);
    put32(file, 17);
    put32(file, 0x4D4F4750);  // MOGP
    put32(file, static_cast<uint32_t>(68 + modr.size()));
    put32(file, 0);  // group name
    put32(file, 0);  // descriptive name
    put32(file, flags);
    file.resize(file.size() + 68 - 12, 0);  // box, portals, batches, fogs, liquid, id...
    file.insert(file.end(), modr.begin(), modr.end());
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

TEST_CASE("a group's MODR lists the doodads standing in it", "[interior_light]") {
    wowee::pipeline::WMOModel model;
    model.groups.resize(1);

    wowee::pipeline::WMOLoader::loadGroup(groupWithDoodadRefs(0x2000, {3, 7, 12}), model, 0);

    REQUIRE(model.groups[0].doodadRefs == std::vector<uint16_t>{3, 7, 12});
}

TEST_CASE("only doodads an interior group lists stand inside", "[interior_light]") {
    wowee::pipeline::WMOModel model;
    model.doodads.resize(5);
    model.groups.resize(2);
    model.groups[0].flags = 0x2000;  // the hall
    model.groups[0].doodadRefs = {0, 3};
    model.groups[1].flags = 0x8;     // the porch outside its door
    model.groups[1].doodadRefs = {1, 2};

    REQUIRE(wowee::pipeline::interiorDoodads(model) ==
            std::vector<bool>{true, false, false, true, false});
}

TEST_CASE("a unit on a dim floor is lit from above, brought up to the client's minimum",
          "[interior_light]") {
    // MOHD 0.1 grey, the floor's MOCV a dim orange: the floor draws with
    // 0.1 + 2 * (0.1, 0.05, 0.0) = (0.3, 0.2, 0.1).
    const InteriorLight light =
        unitInteriorLight(glm::vec3(0.1f), glm::vec3(0.1f, 0.05f, 0.0f));

    // Direct: scaled until its brightest channel is 0xA8 / 255.
    REQUIRE(light.direct.r == Catch::Approx(0.658824f));
    REQUIRE(light.direct.g == Catch::Approx(0.439216f));
    REQUIRE(light.direct.b == Catch::Approx(0.219608f));
    // Ambient: under 0x60 / 255 already, so the floor's colour as it is.
    REQUIRE(light.ambient.r == Catch::Approx(0.3f));
    REQUIRE(light.ambient.g == Catch::Approx(0.2f));
    REQUIRE(light.ambient.b == Catch::Approx(0.1f));
}

TEST_CASE("a unit on a bright floor keeps it as direct light and a capped ambient",
          "[interior_light]") {
    // 0.2 + 2 * (0.3, 0.3, 0.1) = (0.8, 0.8, 0.4).
    const InteriorLight light =
        unitInteriorLight(glm::vec3(0.2f), glm::vec3(0.3f, 0.3f, 0.1f));

    REQUIRE(light.direct.r == Catch::Approx(0.8f));
    REQUIRE(light.direct.g == Catch::Approx(0.8f));
    REQUIRE(light.direct.b == Catch::Approx(0.4f));
    // Scaled until its brightest channel is 0x60 / 255.
    REQUIRE(light.ambient.r == Catch::Approx(0.376471f));
    REQUIRE(light.ambient.g == Catch::Approx(0.376471f));
    REQUIRE(light.ambient.b == Catch::Approx(0.188235f));
}

TEST_CASE("a floor brighter than white lights a unit as white", "[interior_light]") {
    // 0.5 + 2 * 0.6 = 1.7 in every channel, which the floor cannot draw.
    const InteriorLight light = unitInteriorLight(glm::vec3(0.5f), glm::vec3(0.6f));

    REQUIRE(light.direct.r == Catch::Approx(1.0f));
    REQUIRE(light.ambient.r == Catch::Approx(0.376471f));
}

TEST_CASE("a unit with no vertex colour under it is lit by the WMO's ambient alone",
          "[interior_light]") {
    const InteriorLight light = unitInteriorLight(glm::vec3(0.2f, 0.1f, 0.1f), std::nullopt);

    REQUIRE(light.direct.r == Catch::Approx(0.658824f));
    REQUIRE(light.direct.g == Catch::Approx(0.329412f));
    REQUIRE(light.ambient.r == Catch::Approx(0.2f));
    REQUIRE(light.ambient.g == Catch::Approx(0.1f));
}

TEST_CASE("a unit in a black room with a black floor gets no interior light",
          "[interior_light]") {
    const InteriorLight light = unitInteriorLight(glm::vec3(0.0f), glm::vec3(0.0f));

    REQUIRE(light.direct == glm::vec3(0.0f));
    REQUIRE(light.ambient == glm::vec3(0.0f));
}

TEST_CASE("a floor's colour is interpolated between the corners of the triangle hit",
          "[interior_light]") {
    const glm::vec3 a(0, 0, 0), b(4, 0, 0), c(0, 4, 0);
    const glm::vec3 red(1, 0, 0), green(0, 1, 0), blue(0, 0, 1);

    REQUIRE(colorAtPoint(b, a, b, c, red, green, blue) == glm::vec3(0, 1, 0));
    const glm::vec3 midAB = colorAtPoint(glm::vec3(2, 0, 0), a, b, c, red, green, blue);
    REQUIRE(midAB.r == Catch::Approx(0.5f));
    REQUIRE(midAB.g == Catch::Approx(0.5f));
    REQUIRE(midAB.b == Catch::Approx(0.0f).margin(1e-6));
    const glm::vec3 inside = colorAtPoint(glm::vec3(1, 2, 0), a, b, c, red, green, blue);
    REQUIRE(inside.r == Catch::Approx(0.25f));
    REQUIRE(inside.g == Catch::Approx(0.25f));
    REQUIRE(inside.b == Catch::Approx(0.5f));
}

TEST_CASE("a doodad inside is lit by the WMO's ambient and its MODD colour from above",
          "[interior_light]") {
    const glm::vec3 mohd(0.25f, 0.2f, 0.15f);
    const glm::vec3 modd(0.9f, 0.7f, 0.5f);
    const InteriorLightGPU gpu = packInteriorLight(doodadInteriorLight(mohd, modd));
    const glm::vec3 frameAmbient(0.4f, 0.4f, 0.5f);
    const glm::vec3 sun(1.0f, 1.0f, 0.9f);
    const glm::vec3 eveningSunDir(0.9f, 0.0f, -0.436f);

    REQUIRE(interior_light::interiorAmbient(gpu.ambient, frameAmbient) == mohd);
    REQUIRE(interior_light::interiorDirect(gpu.ambient, gpu.direct, sun) == modd);
    // Falls from above along the client's fixed interior direction, not
    // along the low sun outside.
    const glm::vec3 dir = interior_light::interiorLightDir(gpu.ambient, eveningSunDir);
    REQUIRE(dir.x == Catch::Approx(-0.30822f));
    REQUIRE(dir.y == Catch::Approx(-0.30822f));
    REQUIRE(dir.z == Catch::Approx(-0.9f));
}

TEST_CASE("a model outdoors keeps the frame's ambient and the sun", "[interior_light]") {
    const InteriorLightGPU gpu = packInteriorLight(std::nullopt);
    const glm::vec3 frameAmbient(0.4f, 0.4f, 0.5f);
    const glm::vec3 sun(1.0f, 1.0f, 0.9f);
    const glm::vec3 sunDir(0.5f, 0.0f, -0.866f);

    REQUIRE(interior_light::interiorAmbient(gpu.ambient, frameAmbient) == frameAmbient);
    REQUIRE(interior_light::interiorDirect(gpu.ambient, gpu.direct, sun) == sun);
    REQUIRE(interior_light::interiorLightDir(gpu.ambient, sunDir) == sunDir);
}

TEST_CASE("a WMO with an all-zero ambient leaves the frame's ambient on models inside",
          "[interior_light]") {
    const InteriorLightGPU gpu =
        packInteriorLight(doodadInteriorLight(glm::vec3(0.0f), glm::vec3(0.5f)));
    const glm::vec3 frameAmbient(0.4f, 0.4f, 0.5f);

    REQUIRE(interior_light::interiorAmbient(gpu.ambient, frameAmbient) == frameAmbient);
    REQUIRE(interior_light::interiorDirect(gpu.ambient, gpu.direct, glm::vec3(1.0f)) ==
            glm::vec3(0.5f));
}
