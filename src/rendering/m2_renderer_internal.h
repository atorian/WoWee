// m2_renderer_internal.h - shared helpers for the m2_renderer split files.
// All functions are inline to allow inclusion in multiple translation units.
#pragma once

#include "collision/collision_world.hpp"
#include "collision/geometry.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_billboard.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/profiler.hpp"
#include "core/logger.hpp"
#include <mutex>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace rendering {
namespace m2_internal {

// World-space center of a skinned batch. Glow cards on swinging lanterns are
// bone animated; using only modelMatrix * bind-pose center leaves both their
// sprite and the pool of local light fixed at the fixture origin.
inline glm::vec3 animatedBatchWorldCenter(const M2Instance& instance,
                                          const M2ModelGPU::BatchGPU& batch) {
    if (batch.lightBoneAnchors.empty()) {
        return glm::vec3(instance.modelMatrix * glm::vec4(batch.center, 1.0f));
    }

    glm::vec4 animatedCenter(0.0f);
    for (const auto& anchor : batch.lightBoneAnchors) {
        if (anchor.bone < instance.boneMatrices.size()) {
            animatedCenter += instance.boneMatrices[anchor.bone] * anchor.weightedPoint;
        } else {
            animatedCenter += anchor.weightedPoint;
        }
    }
    return glm::vec3(instance.modelMatrix * animatedCenter);
}

// Lanterns are broad point lights, so the small physical displacement of a
// swinging bulb is almost invisible on the ground. Follow the authored hanging
// chain from its fixed suspension pivot to the animated glow tip, then project
// that direction toward the model's ground plane. The glow sprite remains at
// animatedBatchWorldCenter(); only the pool of light uses this projected center.
inline glm::vec3 animatedBatchLightWorldCenter(const M2Instance& instance,
                                               const M2ModelGPU::BatchGPU& batch) {
    glm::vec3 tip = animatedBatchWorldCenter(instance, batch);
    if (batch.lightSuspensionBone == UINT16_MAX ||
        batch.lightSuspensionBone >= instance.boneMatrices.size()) {
        return tip;
    }

    const glm::vec3 suspension = glm::vec3(
        instance.modelMatrix * instance.boneMatrices[batch.lightSuspensionBone] *
        glm::vec4(batch.lightSuspensionPoint, 1.0f));
    const glm::vec3 chain = tip - suspension;
    if (chain.z >= -0.05f) return tip;

    const float groundZ = glm::vec3(instance.modelMatrix[3]).z;
    const float groundDrop = tip.z - groundZ;
    if (groundDrop <= 0.0f) return tip;

    // Deliberately amplify the projected travel. At the authored Darkshire
    // swing amplitude a physically exact pool moves less than one terrain unit,
    // which disappears inside the broad local-light falloff.
    constexpr float kLanternPoolSwingExaggeration = 3.0f;
    glm::vec2 projectedOffset(chain.x, chain.y);
    projectedOffset *= (groundDrop / -chain.z) * kLanternPoolSwingExaggeration;
    const float offsetLength = glm::length(projectedOffset);
    const float maxOffset = std::max(0.75f, 4.0f * instance.scale);
    if (offsetLength > maxOffset) projectedOffset *= maxOffset / offsetLength;
    tip.x += projectedOffset.x;
    tip.y += projectedOffset.y;
    return tip;
}

// ---- Constants ----
/// The size of m2.vert's Push block (M2PushConstants in m2_renderer_render.cpp).
inline constexpr uint32_t kM2PushConstantBytes = 28;

/// m2_particle.frag's push constants.
struct M2ParticlePush {
    float tileX = 1.0f, tileY = 1.0f;
    /// m2AlphaRef of the blend pipeline: a pixel below it is discarded.
    float alphaRef = 0.0f;
    /// M2FogMode: None for an unfogged emitter (flag 0x8).
    int32_t fogMode = 0;
};

// ---- Geometry / collision helpers ----

inline float computeGroundDetailDownOffset(const M2ModelGPU& model, float scale) {
    const float pivotComp = glm::clamp(std::max(0.0f, model.boundMin.z * scale), 0.0f, 0.10f);
    const float terrainSink = 0.03f;
    return pivotComp + terrainSink;
}

/// The model-space box an instance's world bounds are made from: its
/// collision mesh's, or for a model without one - which collides with
/// nothing - its visual bounds.
inline void collisionLocalBounds(const M2ModelGPU& model, glm::vec3& outMin, glm::vec3& outMax) {
    if (model.collision) {
        outMin = model.collision->boundMin;
        outMax = model.collision->boundMax;
    } else {
        outMin = model.boundMin;
        outMax = model.boundMax;
    }
}

using collision::transformAABB;

/// Bone transforms for one instance.
///
/// `view` is the camera's view matrix, which billboard bones turn against;
/// pass nullptr and those bones keep their authored orientation.
inline void computeBoneMatrices(const M2ModelGPU& model, M2Instance& instance,
                                const glm::mat4* view = nullptr) {
    ZoneScopedN("M2::computeBoneMatrices");
    size_t numBones = std::min(model.bones.size(), size_t(kMaxBonesPerInstance));
    if (numBones == 0) return;
    instance.boneMatrices.resize(numBones);
    const auto& gsd = model.globalSequenceDurations;
    // Model space to view space and back, for billboard bones; worked out at
    // the first one.
    bool haveViewModel = false;
    glm::mat4 viewModel(1.0f), invViewModel(1.0f);

    for (size_t i = 0; i < numBones; i++) {
        const auto& bone = model.bones[i];
        const float boneTime = instance.animTime;
        glm::vec3 trans = m2_track::sampleVec3(
            bone.translation, instance.currentSequenceIndex, boneTime,
            instance.globalSequenceTime, gsd, glm::vec3(0.0f));
        glm::quat rot = m2_track::sampleQuat(
            bone.rotation, instance.currentSequenceIndex, boneTime,
            instance.globalSequenceTime, gsd);
        glm::vec3 scl = m2_track::sampleVec3(
            bone.scale, instance.currentSequenceIndex, boneTime,
            instance.globalSequenceTime, gsd, glm::vec3(1.0f));

        if (scl.x < 0.001f) scl.x = 1.0f;
        if (scl.y < 0.001f) scl.y = 1.0f;
        if (scl.z < 0.001f) scl.z = 1.0f;

        const glm::mat4 rotScale = glm::scale(glm::toMat4(rot), scl);
        glm::mat4 local = glm::translate(glm::mat4(1.0f), bone.pivot);
        local = glm::translate(local, trans);
        local *= rotScale;
        local = glm::translate(local, -bone.pivot);

        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones) {
            instance.boneMatrices[i] = instance.boneMatrices[bone.parentBone] * local;
        } else {
            instance.boneMatrices[i] = local;
        }

        // A billboard bone is rebuilt in view space, after its whole parent
        // chain, and brought back into model space; its children follow it.
        // A shield stays upright and turns about its Z axis to the view.
        // Diagnostic: WOWEE_M2_NO_BILLBOARD=1 leaves every billboard bone as
        // authored, which tells a billboard fault from anything else.
        static const bool kNoBillboard = std::getenv("WOWEE_M2_NO_BILLBOARD") != nullptr;
        if (view && !kNoBillboard && (bone.flags & kM2BoneBillboardMask) != 0) {
            if (!haveViewModel) {
                viewModel = *view * instance.modelMatrix;
                invViewModel = glm::inverse(viewModel);
                haveViewModel = true;
            }
            const glm::mat4 boneView = viewModel * instance.boneMatrices[i];
            const glm::mat4 billboarded = m2BillboardBone(bone.flags, boneView, glm::mat3(rotScale), bone.pivot);
            instance.boneMatrices[i] = invViewModel * billboarded;
            // Diagnostic: WOWEE_M2_BILLBOARD_DIAG=1 says, about once a second
            // per spell effect model, that its billboard bones are turned and
            // which way they ended up facing in view space.
            static const bool kBillboardDiag = std::getenv("WOWEE_M2_BILLBOARD_DIAG") != nullptr;
            if (kBillboardDiag && model.isSpellEffect) {
                static std::mutex diagMutex;
                static std::unordered_map<std::string, std::chrono::steady_clock::time_point> saidAt;
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(diagMutex);
                auto& last = saidAt[model.name];
                if (now - last > std::chrono::seconds(1)) {
                    last = now;
                    const glm::vec3 x0(boneView[0]), x1(billboarded[0]);
                    LOG_WARNING("Billboard: '", model.name, "' instance ", instance.id, " bone ", i,
                                " flags 0x", std::hex, bone.flags, std::dec, " X in view (",
                                x0.x, ", ", x0.y, ", ", x0.z, ") -> (", x1.x, ", ", x1.y, ", ", x1.z, ")");
                }
            }
        }
    }
    instance.bonesDirty[0] = instance.bonesDirty[1] = true;
}

// Firelight guttering for lamps, torches and braziers.
//
// The phase is hashed from the fixture's world position so no two lights ever
// pulse together - a synchronised row of street lamps reads as a rendering
// artifact rather than firelight - and it stays stable frame to frame because
// it derives from a fixed position rather than a counter.
//
// Two detuned sines with periods of roughly 3.7 and 1.6 seconds, sharing no
// common multiple over any watchable span. Slower than this and the eye adapts
// to the change and stops seeing it at all - a 15-second swell is invisible no
// matter how deep it goes - while faster reads as a strobe.
//
// phaseSeed must be a position that does not move frame to frame - the fixture's
// placement, never an animated bone centre or a sprite offset toward the camera.
// The hash is chaotic by design, so a seed that drifts even slightly re-rolls the
// phase every frame and the result is a strobe rather than a flicker. It is also
// quantised to a one-unit grid here as insurance against a caller passing
// something that creeps.
inline float lampFlicker(const glm::vec3& phaseSeed, float seconds,
                         float base, float slowAmp, float fastAmp) {
    const glm::vec3 cell = glm::floor(phaseSeed);
    float h = std::sin(cell.x * 12.9898f + cell.y * 78.233f +
                       cell.z * 37.719f) * 43758.5453f;
    const float phase = (h - std::floor(h)) * 6.2831853f;
    return base + slowAmp * std::sin(seconds * 1.70f + phase)
                + fastAmp * std::sin(seconds * 3.90f + phase * 1.7f);
}

/// Seconds since process start, shared by the flicker animations.
inline float lampFlickerClockSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

// WOWEE_SKY_MIP_BIAS pushes the sky's textures toward smaller mips.
//
// Every per-frame quantity behind the sky has now been measured and holds
// still while it flickers: the lighting, the clock, the culling, the frame
// time, and every one of the thirty-four batches' material flags. Nothing
// is alpha tested, colour keyed, or drawn as a glow card. What is left is
// the texture sample, and the sky is the one model where that is loud:
// eighteen of its layers are additive, so each layer's sampling noise adds
// to the last rather than replacing it, and the dome is at its most
// foreshortened exactly where a turn sweeps fastest.
//
// A knob rather than a fixed value, because if this is the cause the right
// amount is a thing to measure and not to guess.
inline float m2SkyMipBias() {
    static const float bias = [] {
        const char* set = std::getenv("WOWEE_SKY_MIP_BIAS");
        return set ? std::strtof(set, nullptr) : 0.0f;
    }();
    return bias;
}

} // namespace m2_internal

// Pull all symbols into the rendering namespace so existing code compiles unchanged
using namespace m2_internal;

} // namespace rendering
} // namespace wowee
