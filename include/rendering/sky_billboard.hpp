#pragma once

/// Where a sun or moon quad stands in the sky, facing the eye.
///
/// The bodies are drawn with the view's translation removed, so the eye is at
/// the origin and a body at `position` is seen along that direction. The quad
/// was translated there and scaled in x and y and left in the world's
/// horizontal plane: a floor tile hung in the sky, seen from below at the
/// body's elevation, so the disc came out squashed to sin(elevation) of its
/// height - a flat oval for a sun at 37 degrees, a sliver near the horizon.
///
/// The quad's plane is turned square to the line from the eye, its up as close
/// to the world's as that allows, so the disc is round wherever the body is.

#include <cmath>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// The model matrix for the celestial quad (corners at +-0.5 in x and y) of a
/// body at `position`, `size` across, facing the origin.
inline glm::mat4 skyBillboard(const glm::vec3& position, float size) {
    const glm::vec3 toEye = -glm::normalize(position);
    // World up, unless the body is straight overhead or underfoot, where any
    // horizontal axis will do.
    const glm::vec3 reference =
        std::abs(toEye.z) > 0.999f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 right = glm::normalize(glm::cross(reference, toEye));
    const glm::vec3 up = glm::cross(toEye, right);

    glm::mat4 model(1.0f);
    model[0] = glm::vec4(right * size, 0.0f);
    model[1] = glm::vec4(up * size, 0.0f);
    model[2] = glm::vec4(toEye, 0.0f);
    model[3] = glm::vec4(position, 1.0f);
    return model;
}

}  // namespace wowee::rendering
