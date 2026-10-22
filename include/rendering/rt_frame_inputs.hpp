#pragma once
#include <glm/glm.hpp>
namespace wowee::rendering {
struct RtFrameInputs {
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 cameraPos;
        glm::vec3 sunDir;      // toward the sun
        glm::vec3 sunColor;
        glm::vec3 skyColor;    // what the flat ambient term stands for
        bool sunUp = true;
        float distance = 0; // standalone probes can trace without a camera limit
        /// Opt in only when a material consumer will read mirror radiance.
        /// The client currently has none; the reflection GPU probe does.
        bool traceReflections = false;
    };
}
