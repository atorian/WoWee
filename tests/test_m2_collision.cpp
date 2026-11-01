#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include "collision/m2_shape.hpp"
#include "collision/m2_facets.hpp"
#include <cstring>
#include <vector>

TEST_CASE("M2 collision normals retain consecutive authored vectors", "[m2][collision]") {
    std::vector<uint8_t> bytes(352 + 24, 0);
    auto put32 = [&](size_t at, uint32_t value) { std::memcpy(bytes.data()+at, &value, 4); };
    std::memcpy(bytes.data(), "MD20", 4);
    put32(4, 264);
    put32(0xd8, 6); put32(0xdc, 340);
    put32(0xe0, 3); put32(0xe4, 304);
    put32(0xe8, 2); put32(0xec, 352);
    const float positions[] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t indices[] = {0,1,2, 2,1,0};
    std::memcpy(bytes.data()+304, positions, sizeof(positions));
    std::memcpy(bytes.data()+340, indices, sizeof(indices));
    // Two normals end exactly at EOF. There is no fourth plane component.
    const float authored[] = {0, 0, 1, 0.6f, 0.8f, 0};
    std::memcpy(bytes.data()+352, authored, sizeof(authored));
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.collisionNormals.size() == 2);
    CHECK(model.collisionNormals[0].x == 0);
    CHECK(model.collisionNormals[0].y == 0);
    CHECK(model.collisionNormals[0].z == 1);
    CHECK(model.collisionNormals[1].x == 0.6f);
    CHECK(model.collisionNormals[1].y == 0.8f);
    CHECK(model.collisionNormals[1].z == 0);
    const auto shape = wowee::collision::M2Shape::build("authored", model.collisionVertices,
        model.collisionIndices, model.collisionNormals);
    REQUIRE(shape);
    const auto facets = wowee::collision::queryM2Facets(shape->vertices, shape->indices,
        shape->faceNormals, glm::mat4(1), glm::vec3(-2), glm::vec3(2), 0);
    REQUIRE(facets.size() == 2);
    // The reversed face keeps its authored normal. Recomputing a cross product
    // here would turn it into a downward floor instead of the authored plane.
    CHECK(facets[1].plane.normal.x == 0.6f);
    CHECK(facets[1].plane.normal.y == 0.8f);
    CHECK(facets[1].plane.normal.z == 0);
}
