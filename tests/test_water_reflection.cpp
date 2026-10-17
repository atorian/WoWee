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
