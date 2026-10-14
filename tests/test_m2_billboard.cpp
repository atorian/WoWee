// M2 billboard bones against the client's bone update (WoW.exe 3.3.5a
// 0x82f0f0, dispatch 0x82ff3a): rebuilt in view space, each axis keeping its
// length and the pivot kept where it was.
#include <catch_amalgamated.hpp>

#include "rendering/m2_billboard.hpp"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::rendering;

namespace {

glm::vec3 axis(const glm::mat4& m, int i) { return glm::vec3(m[i]); }

} // namespace

TEST_CASE("A bone that is no billboard keeps its matrix", "[m2][billboard]") {
    const glm::mat4 bone = glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(m2BillboardBone(0x200, bone, glm::mat3(1.0f), glm::vec3(0.0f)) == bone);
}

TEST_CASE("A spherical billboard faces the view whatever turned it", "[m2][billboard]") {
    // Turned and scaled by its parents, still +X at the viewer, Y right, Z up,
    // at its scale.
    const glm::mat4 bone = glm::scale(glm::rotate(glm::mat4(1.0f), 1.1f, glm::vec3(1.0f, 2.0f, 0.5f)),
                                      glm::vec3(2.0f));
    const glm::mat4 out = m2BillboardBone(kM2BoneSphericalBillboard, bone, glm::mat3(1.0f), glm::vec3(0.0f));
    CHECK(axis(out, 0).z == Catch::Approx(2.0f));
    CHECK(axis(out, 1).x == Catch::Approx(2.0f));
    CHECK(axis(out, 2).y == Catch::Approx(2.0f));
}

TEST_CASE("A Z-locked billboard keeps its Z and turns to the view", "[m2][billboard]") {
    // Mana Shield's root bone, 0x240: upright on screen, whichever way the
    // unit carrying it faces.
    const glm::mat4 facing = glm::rotate(glm::mat4(1.0f), 2.0f, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 bone = facing * glm::rotate(glm::mat4(1.0f), -glm::half_pi<float>(),
                                                glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::mat4 out = m2BillboardBone(0x240, bone, glm::mat3(1.0f), glm::vec3(0.0f));
    CHECK(glm::length(axis(out, 2) - axis(bone, 2)) == Catch::Approx(0.0f).margin(1e-5));
    CHECK(axis(out, 1).z == Catch::Approx(0.0f).margin(1e-6));
    CHECK(glm::determinant(glm::mat3(out)) == Catch::Approx(1.0f));
}

TEST_CASE("A billboard keeps its pivot where it was", "[m2][billboard]") {
    const glm::vec3 pivot(0.2f, 0.0f, 1.1f);
    const glm::mat4 bone = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -1.0f, -8.0f)) *
                           glm::rotate(glm::mat4(1.0f), 0.4f, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::mat4 out = m2BillboardBone(kM2BoneCylindricalLockZ, bone, glm::mat3(1.0f), pivot);
    const glm::vec3 before = glm::vec3(bone * glm::vec4(pivot, 1.0f));
    const glm::vec3 after = glm::vec3(out * glm::vec4(pivot, 1.0f));
    CHECK(glm::length(after - before) == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("An axis along the view collapses the card, as in the client", "[m2][billboard]") {
    const glm::mat4 bone = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    // Its X now lies along the view axis: nothing to turn about.
    const glm::mat4 out = m2BillboardBone(kM2BoneCylindricalLockX, bone, glm::mat3(1.0f), glm::vec3(0.0f));
    CHECK(glm::length(axis(out, 1)) == Catch::Approx(0.0f).margin(1e-6));
}
