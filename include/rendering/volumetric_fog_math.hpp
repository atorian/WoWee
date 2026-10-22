#pragma once

/// The geometry the fog volume is laid out in, apart from anything Vulkan so a
/// test can hold it against the projection the shaders use. See VolumetricFog.

#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::rendering {

/// The rays from the camera through the four corners of the view, each one
/// yard deep along it, for uv (0,0), (1,0), (0,1) and (1,1) - uv being
/// clip.xy / clip.w * 0.5 + 0.5, which is how every shader finds its place in
/// the volume. A perspective projection makes screen position linear in these,
/// so the ray through any uv is their bilinear mix, and the point d yards deep
/// along the view there is the camera plus d times that ray.
///
/// Inverted from the projection rather than rebuilt from a field of view, so
/// the upscaler's jitter and the flipped Y come along with it. The view must be
/// a rotation and a translation, as a look-at is.
inline std::array<glm::vec3, 4> froxelCornerRays(const glm::mat4& view, const glm::mat4& projection) {
    const glm::mat4 invProj = glm::inverse(projection);
    const glm::mat3 invViewRot = glm::transpose(glm::mat3(view));
    const glm::vec2 corners[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    std::array<glm::vec3, 4> rays{};
    for (int i = 0; i < 4; i++) {
        const glm::vec2 ndc = corners[i] * 2.0f - 1.0f;
        const glm::vec4 v = invProj * glm::vec4(ndc, 0.5f, 1.0f);
        glm::vec3 viewRay = glm::vec3(v) / v.w;
        viewRay /= -viewRay.z;  // one yard in front, along -Z
        rays[i] = invViewRot * viewRay;
    }
    return rays;
}

/// The radical inverse of `index` in `base`: the Halton sequence.
inline float halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

/// Where in its cell the inject pass samples on frame `frame`, in cells: a
/// different point every frame, sixteen of them in turn.
inline glm::vec3 fogCellJitter(uint32_t frame) {
    const uint32_t seq = (frame % 16) + 1;
    return {halton(seq, 2), halton(seq, 3), halton(seq, 5)};
}

/// Whether the view is where it was last frame, near enough that every cell
/// is still the same stretch of air. The corner rays carry the turn and the
/// projection: they are one yard deep, so a ten-thousandth of a yard is about
/// a tenth of a pixel. The camera's position is held to a hundredth of a
/// yard - well inside the smallest cell - since a follow camera that eases
/// toward its target may never land on it exactly.
inline bool fogViewHeldStill(const std::array<glm::vec3, 4>& rays,
                             const std::array<glm::vec3, 4>& prevRays,
                             const glm::vec3& cameraPos, const glm::vec3& prevCameraPos) {
    if (glm::length(cameraPos - prevCameraPos) > 0.01f) return false;
    for (size_t i = 0; i < rays.size(); i++) {
        if (glm::length(rays[i] - prevRays[i]) > 1e-4f) return false;
    }
    return true;
}

/// How much of a cell's value is last frame's, given how many frames in a row
/// the view has held still.
///
/// A cell takes one sample of the sun a frame - lit or in shadow, nothing
/// between - at sixteen points in turn. Keeping nine tenths of the old value
/// is short enough to follow a moving view without smearing it, but it never
/// settles on the average of the sixteen: a cell across a shadow edge swings
/// by several percent from frame to frame for as long as the view holds, and
/// looking toward the sun, where the air takes a dozen times the light, the
/// whole of the sunlit mist flickers. So once the view stops, the old value
/// counts for more each frame, up to 63/64 after about a second at sixty
/// frames, where the swing is a sixth of what it was. The moment the view
/// moves it is back to nine tenths.
inline float fogHistoryWeight(uint32_t framesStill) {
    constexpr uint32_t kMovingSamples = 10;
    constexpr uint32_t kStillSamples = 64;
    const uint32_t samples = std::min(kMovingSamples + std::min(framesStill, kStillSamples),
                                      kStillSamples);
    return 1.0f - 1.0f / static_cast<float>(samples);
}

}  // namespace wowee::rendering
