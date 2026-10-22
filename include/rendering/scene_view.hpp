#pragma once

#include "rendering/camera.hpp"
#include "rendering/water_reflection.hpp"

namespace wowee::rendering {

/// One view's immutable camera data, shared by extraction and recording.
/// Reflection and shadow views must never borrow the primary view's cull.
struct SceneView {
    glm::mat4 view{1};
    glm::mat4 projection{1};
    glm::vec3 position{0};

    static SceneView fromCamera(const Camera& camera) {
        return {camera.getViewMatrix(),camera.getProjectionMatrix(),camera.getPosition()};
    }
    static SceneView reflection(const Camera& camera, float height) {
        SceneView result;
        result.position=camera.getPosition();
        result.position.z=2.f*height-result.position.z;
        result.view=mirroredView(camera.getPosition(),camera.getForward(),height);
        result.projection=obliqueReflectionProjection(camera.getProjectionMatrix(),result.view,height);
        return result;
    }
    glm::mat4 viewProjection() const { return projection*view; }
};

} // namespace wowee::rendering
