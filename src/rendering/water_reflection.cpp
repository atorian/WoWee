#include "rendering/water_reflection.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

namespace wowee {
namespace rendering {

glm::mat4 mirroredView(glm::vec3 cameraPos, glm::vec3 forward, float waterHeight) {
    glm::vec3 mirroredPos = cameraPos;
    mirroredPos.z = 2.0f * waterHeight - cameraPos.z;
    forward.z = -forward.z;
    return glm::lookAt(mirroredPos, mirroredPos + forward, glm::vec3(0.0f, 0.0f, 1.0f));
}

glm::mat4 obliqueReflectionProjection(const glm::mat4& projection, const glm::mat4& view,
                                      float waterHeight) {
    // The plane z = waterHeight, facing up, in the mirrored camera's space.
    const glm::vec4 planeWorld(0.0f, 0.0f, 1.0f, -waterHeight);
    const glm::vec4 c = glm::transpose(glm::inverse(view)) * planeWorld;

    // Lengyel's oblique near plane. Clip depth here runs from 0 at the near
    // plane to w at the far one, so the depth row becomes the plane itself,
    // scaled so that the far corner of the frustum on the plane's side still
    // lands on the far plane: Q is that corner, back from clip space. The
    // form taken here before was the one for -w to w depth, which put the
    // near plane nowhere near the water, and the bed under the sea came
    // through in the reflection as dark ground. The corner is picked by the
    // plane's signs in clip space, not the camera's: this projection flips y.
    const glm::mat4 inverseProjection = glm::inverse(projection);
    const glm::vec4 cClip = glm::transpose(inverseProjection) * c;
    const glm::vec4 q = inverseProjection *
                        glm::vec4(glm::sign(cClip.x), glm::sign(cClip.y), 1.0f, 1.0f);
    const glm::vec4 m = c * (1.0f / glm::dot(c, q));
    glm::mat4 result = projection;
    result[0][2] = m.x;
    result[1][2] = m.y;
    result[2][2] = m.z;
    result[3][2] = m.w;
    return result;
}

} // namespace rendering
} // namespace wowee
