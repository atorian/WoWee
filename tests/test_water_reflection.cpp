// The water's planar reflection: the mirrored camera sees what stands above
// the water and nothing of the bed under it.
#include <catch_amalgamated.hpp>

#include "rendering/water_reflection.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::mirroredView;
using wowee::rendering::obliqueReflectionProjection;

namespace {

/// The camera's projection as Camera builds it: zero-to-one depth, y down.
glm::mat4 cameraProjection() {
    glm::mat4 p = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 3000.0f);
    p[1][1] *= -1.0f;
    return p;
}

/// Whether a world point survives the clip volume of `viewProj`.
bool drawn(const glm::mat4& viewProj, glm::vec3 p) {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    return c.w > 0.0f && c.x >= -c.w && c.x <= c.w && c.y >= -c.w && c.y <= c.w &&
           c.z >= 0.0f && c.z <= c.w;
}

}  // namespace

TEST_CASE("the reflection draws what stands above the water", "[water][reflection]") {
    const float water = 0.0f;
    const glm::vec3 eye(0.0f, 0.0f, 10.0f);
    const glm::vec3 forward = glm::normalize(glm::vec3(0.0f, 1.0f, -0.3f));
    const glm::mat4 view = mirroredView(eye, forward, water);
    const glm::mat4 proj = obliqueReflectionProjection(cameraProjection(), view, water);
    // A mast forty yards ahead, from the water up.
    for (float z : {0.5f, 5.0f, 20.0f}) CHECK(drawn(proj * view, glm::vec3(0.0f, 40.0f, z)));
}

TEST_CASE("the reflection draws nothing of the bed under the water", "[water][reflection]") {
    const float water = 0.0f;
    const glm::vec3 eye(0.0f, 0.0f, 10.0f);
    const glm::vec3 forward = glm::normalize(glm::vec3(0.0f, 1.0f, -0.3f));
    const glm::mat4 view = mirroredView(eye, forward, water);
    const glm::mat4 proj = obliqueReflectionProjection(cameraProjection(), view, water);
    // The sea bed and a rock on it, ahead of the camera and in the mirrored
    // frustum: a mirror on the surface does not show them.
    for (float y : {20.0f, 40.0f, 80.0f}) {
        for (float z : {-0.5f, -3.0f, -12.0f}) CHECK_FALSE(drawn(proj * view, glm::vec3(0.0f, y, z)));
    }
}

// ---------------------------------------------------------------------------
// Which water the reflection is drawn for, and what is drawn into it.

using wowee::rendering::ReflectedCamera;
using wowee::rendering::WaterInView;
using wowee::rendering::waterReflectionView;

namespace {

/// A camera ten yards over the water at z = 0, looking along +y and a little down.
ReflectedCamera cameraOverWater(float eyeHeight = 10.0f) {
    ReflectedCamera c;
    c.position = glm::vec3(0.0f, 0.0f, eyeHeight);
    c.forward = glm::normalize(glm::vec3(0.0f, 1.0f, -0.3f));
    c.view = glm::lookAt(c.position, c.position + c.forward, glm::vec3(0.0f, 0.0f, 1.0f));
    c.projection = cameraProjection();
    return c;
}

/// A pool from x0..x1, y0..y1, lying at `height`.
WaterInView pool(float x0, float x1, float y0, float y1, float height) {
    return {glm::vec3(x0, y0, height - 0.2f), glm::vec3(x1, y1, height + 0.2f), height};
}

}  // namespace

TEST_CASE("the reflection is drawn for the water that fills most of the picture", "[water][reflection]") {
    // A wide lake at the camera's feet and a small basin on a terrace further
    // off, four yards higher.
    const WaterInView water[] = {pool(-2.0f, 2.0f, 60.0f, 63.0f, 4.0f),
                                 pool(-60.0f, 60.0f, 10.0f, 120.0f, 0.0f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    CHECK(mirror->height == 0.0f);
    CHECK(mirror->mirrors(water[1]));
    // The basin lies on another plane and reflects the sky only: the lake's
    // mirror would show it the wrong place.
    CHECK_FALSE(mirror->mirrors(water[0]));
}

TEST_CASE("the chunks of one lake share its reflection", "[water][reflection]") {
    const WaterInView water[] = {pool(-20.0f, 0.0f, 20.0f, 60.0f, 0.0f),
                                 pool(0.0f, 20.0f, 20.0f, 60.0f, 0.3f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    CHECK(mirror->mirrors(water[0]));
    CHECK(mirror->mirrors(water[1]));
}

TEST_CASE("a building whose mirror image falls in the pool is drawn into the reflection",
          "[water][reflection]") {
    const WaterInView water[] = {pool(-5.0f, 5.0f, 20.0f, 40.0f, 0.0f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    // A tower just past the pool, in line with it.
    CHECK(mirror->canSee(glm::vec3(-1.0f, 45.0f, 0.0f), glm::vec3(1.0f, 47.0f, 30.0f)));
}

TEST_CASE("a building whose mirror image falls on dry ground is not drawn into the reflection",
          "[water][reflection]") {
    const WaterInView water[] = {pool(-5.0f, 5.0f, 20.0f, 40.0f, 0.0f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    // A tower well to the side, in the camera's view but nowhere over the
    // pool: its image in a mirror the size of the screen lies beside it.
    CHECK_FALSE(mirror->canSee(glm::vec3(-36.0f, 45.0f, 0.0f), glm::vec3(-34.0f, 47.0f, 30.0f)));
    // Behind the camera.
    CHECK_FALSE(mirror->canSee(glm::vec3(-1.0f, -40.0f, 0.0f), glm::vec3(1.0f, -38.0f, 30.0f)));
}

TEST_CASE("nothing under the water is drawn into its reflection", "[water][reflection]") {
    const WaterInView water[] = {pool(-5.0f, 5.0f, 20.0f, 40.0f, 0.0f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    // A rock on the bed, under the pool.
    CHECK_FALSE(mirror->canSee(glm::vec3(-1.0f, 28.0f, -6.0f), glm::vec3(1.0f, 30.0f, -1.0f)));
    CHECK_FALSE(mirror->canSee(glm::vec3(0.0f, 29.0f, -4.0f), 2.0f));
}

TEST_CASE("the reflection is drawn only where the water shows it", "[water][reflection]") {
    // A pool off to the left, nearer the bottom of the picture than the top.
    const WaterInView water[] = {pool(-14.0f, -8.0f, 20.0f, 26.0f, 0.0f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    CHECK(mirror->area.z < 0.5f);
    CHECK((mirror->area.z - mirror->area.x) * (mirror->area.w - mirror->area.y) < 0.25f);
}

TEST_CASE("water above the camera holds no reflection", "[water][reflection]") {
    // Under a pond's surface, looking at it from below.
    const WaterInView water[] = {pool(-20.0f, 20.0f, 0.0f, 40.0f, 12.0f)};
    CHECK_FALSE(waterReflectionView(cameraOverWater(), water));
}

TEST_CASE("standing in a lake, the water near the feet still reflects", "[water][reflection]") {
    // The lake runs on behind the camera; only the part ahead is in the picture.
    const WaterInView water[] = {pool(-60.0f, 60.0f, -50.0f, 60.0f, 0.0f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    // A post in the water a few yards ahead, at the bottom of the picture.
    CHECK(mirror->canSee(glm::vec3(-0.5f, 12.0f, 0.0f), glm::vec3(0.5f, 13.0f, 1.5f)));
}

TEST_CASE("water hidden behind something holds no reflection", "[water][reflection]") {
    // Canals under a city's streets, in the camera's view and covered by
    // them, and a fountain basin up on a terrace that the camera does see.
    WaterInView canals = pool(-60.0f, 60.0f, 10.0f, 120.0f, 0.0f);
    canals.seen = false;
    const WaterInView water[] = {canals, pool(-2.0f, 2.0f, 30.0f, 33.0f, 4.0f)};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    CHECK(mirror->height == 4.0f);
}

TEST_CASE("with no water seen there is no reflection", "[water][reflection]") {
    WaterInView canals = pool(-60.0f, 60.0f, 10.0f, 120.0f, 0.0f);
    canals.seen = false;
    const WaterInView water[] = {canals};
    CHECK_FALSE(waterReflectionView(cameraOverWater(), water));
}

TEST_CASE("the reflection is drawn where the water seen shows it, not the hidden water beside it",
          "[water][reflection]") {
    // One plane: a pool seen off to the left and a stretch of the same water
    // behind a wall on the right.
    WaterInView hidden = pool(8.0f, 14.0f, 20.0f, 26.0f, 0.0f);
    hidden.seen = false;
    const WaterInView water[] = {pool(-14.0f, -8.0f, 20.0f, 26.0f, 0.0f), hidden};
    const auto mirror = waterReflectionView(cameraOverWater(), water);
    REQUIRE(mirror);
    CHECK(mirror->area.z < 0.5f);
}
