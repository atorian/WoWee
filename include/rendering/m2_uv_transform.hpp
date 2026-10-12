#pragma once

/// An M2 texture transform, as the vertex shader applies it to a UV.
///
/// A texture animation carries a translation, a rotation and a scale track.
/// Only the translation used to reach the shader, as a plain offset, so a
/// swirling portal or a turning rune stood still and a pulsing texture never
/// pulsed. The rotation and the scale turn about the texture's centre.

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// uv' = (dot(row0.xy, uv) + row0.z, dot(row1.xy, uv) + row1.z). The w of each
/// row is padding, so the pair packs as two std430 vec4s.
struct M2UVTransform {
    glm::vec4 row0{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 row1{0.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(M2UVTransform) == 32, "two vec4 rows, as the shader reads them");

inline glm::vec2 m2ApplyUVTransform(const M2UVTransform& t, glm::vec2 uv) {
    return {t.row0.x * uv.x + t.row0.y * uv.y + t.row0.z,
            t.row1.x * uv.x + t.row1.y * uv.y + t.row1.z};
}

inline bool m2UVTransformIsIdentity(const M2UVTransform& t) {
    return t.row0 == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f) &&
           t.row1 == glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
}

inline M2UVTransform m2UVTranslation(glm::vec2 offset) {
    M2UVTransform t;
    t.row0.z = offset.x;
    t.row1.z = offset.y;
    return t;
}

/// Translate, then scale and rotate about the texture's centre.
inline M2UVTransform m2UVTransform(const glm::vec3& translation, const glm::quat& rotation,
                                   const glm::vec3& scale) {
    const glm::vec3 pivot(0.5f, 0.5f, 0.0f);
    const glm::mat4 identity(1.0f);
    const glm::mat4 m = glm::translate(identity, pivot) * glm::mat4_cast(rotation) *
                        glm::scale(identity, scale) * glm::translate(identity, -pivot) *
                        glm::translate(identity, translation);
    // glm is column-major: m[column][row].
    M2UVTransform t;
    t.row0 = glm::vec4(m[0][0], m[1][0], m[3][0], 0.0f);
    t.row1 = glm::vec4(m[0][1], m[1][1], m[3][1], 0.0f);
    return t;
}

/// The transform a texture animation stands at for one instance's clock.
inline M2UVTransform m2SampleUVTransform(const pipeline::M2TextureTransform& transform,
                                         int sequenceIndex, float animTimeMs,
                                         float globalTimeMs,
                                         const std::vector<uint32_t>& globalSequenceDurations) {
    const glm::vec3 translation = m2_track::sampleVec3(
        transform.translation, sequenceIndex, animTimeMs, globalTimeMs,
        globalSequenceDurations, glm::vec3(0.0f));
    const glm::quat rotation = m2_track::sampleQuat(
        transform.rotation, sequenceIndex, animTimeMs, globalTimeMs,
        globalSequenceDurations);
    const glm::vec3 scale = m2_track::sampleVec3(
        transform.scale, sequenceIndex, animTimeMs, globalTimeMs,
        globalSequenceDurations, glm::vec3(1.0f));
    return m2UVTransform(translation, rotation, scale);
}

/// The texture animation a combo-table entry names, or none.
inline const pipeline::M2TextureTransform* m2TextureTransformAt(
    const std::vector<uint16_t>& lookup,
    const std::vector<pipeline::M2TextureTransform>& transforms, size_t lookupIndex) {
    if (lookupIndex >= lookup.size()) return nullptr;
    const uint16_t index = lookup[lookupIndex];
    return index < transforms.size() ? &transforms[index] : nullptr;
}

}  // namespace wowee::rendering
