#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace wowee {
namespace rendering {

/// Where one drawing of the sea bed's map lies and what its depth means
/// (SeaBedMap).
struct SeaBedFrame {
    glm::vec2 centre{0.0f};   ///< world xy under the map's middle
    float halfExtent = 0.0f;  ///< yards from the middle to an edge
    float ceiling = 0.0f;     ///< world height of depth 0; nothing above it is drawn
    float range = 0.0f;       ///< yards from the ceiling down to depth 1

    /// World to the map's clip space, looking straight down; the map's
    /// texture coordinate of a point is its clip xy times a half, plus a half.
    [[nodiscard]] glm::mat4 viewProjection() const {
        return glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.0f, range) * lookingDown();
    }
    /// The same footprint, its depth turned over: 0 at the range's foot and
    /// 1 at the ceiling, so the nearest surface a depth test keeps is the
    /// lowest - the buildings seen from below.
    [[nodiscard]] glm::mat4 viewProjectionFromBelow() const {
        return glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, range, 0.0f) * lookingDown();
    }
    /// The world height a depth read from above stands for.
    [[nodiscard]] float heightAt(float depth) const { return ceiling - depth * range; }

private:
    [[nodiscard]] glm::mat4 lookingDown() const {
        const glm::vec3 eye(centre, ceiling);
        return glm::lookAt(eye, eye - glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

/// The frame to draw for a camera at `nearby`, under a ceiling at `ceiling`:
/// centred on the texel corner nearest it, so that a camera moving less than
/// a texel draws the same texels and the waves do not shimmer along the
/// shore as it does.
[[nodiscard]] inline SeaBedFrame seaBedFrameFor(glm::vec2 nearby, float ceiling, float halfExtent,
                                                float range, int texels) {
    const float texel = 2.0f * halfExtent / static_cast<float>(texels);
    SeaBedFrame frame;
    frame.centre = glm::vec2(std::round(nearby.x / texel), std::round(nearby.y / texel)) * texel;
    frame.halfExtent = halfExtent;
    frame.ceiling = ceiling;
    frame.range = range;
    return frame;
}

} // namespace rendering
} // namespace wowee
