// What lights a model standing inside a WMO interior group: which groups count
// as inside, which doodads the file puts inside them, where the building's
// ambient comes from, and what that ambient replaces in the shaders.

#include <catch_amalgamated.hpp>

#include "pipeline/wmo_loader.hpp"
#include "rendering/wmo_group_containment.hpp"

#include <glm/glm.hpp>

#include <cstring>
#include <vector>

namespace interior_light {
using glm::vec3;
using glm::vec4;
#include "../assets/shaders/interior_light.glsli"
}

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

TEST_CASE("a model inside an interior group is lit by the WMO's ambient and not by the sun",
          "[interior_light]") {
    const glm::vec4 inHall(0.25f, 0.2f, 0.15f, 1.0f);
    const glm::vec3 zoneAmbient(0.4f, 0.4f, 0.5f);

    REQUIRE(interior_light::interiorAmbient(inHall, zoneAmbient) ==
            glm::vec3(0.25f, 0.2f, 0.15f));
    REQUIRE(interior_light::interiorSunScale(inHall) == 0.0f);
}

TEST_CASE("a model outdoors keeps the frame's ambient and the sun", "[interior_light]") {
    const glm::vec4 outdoors(0.0f);
    const glm::vec3 zoneAmbient(0.4f, 0.4f, 0.5f);

    REQUIRE(interior_light::interiorAmbient(outdoors, zoneAmbient) ==
            glm::vec3(0.4f, 0.4f, 0.5f));
    REQUIRE(interior_light::interiorSunScale(outdoors) == 1.0f);
}

TEST_CASE("a WMO with an all-zero ambient leaves the frame's ambient on models inside",
          "[interior_light]") {
    const glm::vec4 inBlackHall(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec3 zoneAmbient(0.4f, 0.4f, 0.5f);

    REQUIRE(interior_light::interiorAmbient(inBlackHall, zoneAmbient) ==
            glm::vec3(0.4f, 0.4f, 0.5f));
    REQUIRE(interior_light::interiorSunScale(inBlackHall) == 0.0f);
}
