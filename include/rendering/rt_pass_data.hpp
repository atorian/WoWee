#pragma once
#include <glm/glm.hpp>

namespace wowee::rendering {
// Must match RtPassData in rt_common.glsli.
struct RtPassData {
    glm::mat4 invViewProj;
    glm::mat4 prevViewProj;
    glm::vec4 cameraPos;
    glm::vec4 prevCameraPos;
    glm::vec4 sunDir;
    glm::vec4 sunColor;
    glm::vec4 skyColor;
    glm::vec4 params;
    glm::vec4 extent;
    glm::vec4 sceneExtent;
    glm::vec4 misc;
};

}
