#include "rendering/shadow_params.hpp"
#include "rendering/m2_material.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_sky_material.hpp"
#include "rendering/m2_sway.hpp"
#include "rendering/m2_blend_mode.hpp"
#include "rendering/m2_glow_card.hpp"
#include "rendering/m2_particle_emission.hpp"
#include "core/thread_pool.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/hiz_system.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/cpu_stage_times.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/render_constants.hpp"
#include "rendering/m2_view_distance.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <chrono>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <limits>
#include <future>
#include <iterator>
#include <span>
#include <thread>

#include <set>
#include <utility>

namespace wowee {
namespace rendering {

/// Starts a new instance's animation and gives it bones to draw with now.
///
/// Both spawn paths need this: the one that takes a position and the one that
/// takes a whole matrix, and each had its own copy.
///
/// The bone seed is what keeps a new instance from being invisible for a
/// frame. Bones are computed in update(), so an instance spawned mid-frame has
/// none until the next one; copying them from a sibling of the same model
/// draws it immediately. A seed entry pointing at an instance that has since
/// gone is dropped rather than followed.
void M2Renderer::seedInstanceAnimation(const M2ModelGPU& model, uint32_t modelId,
                                       M2Instance& instance) {
        if (!model.sequences.empty()) {
            instance.currentSequenceIndex = 0;
            instance.idleSequenceIndex = 0;
            instance.animDuration = static_cast<float>(model.sequences[0].duration);
            instance.animTime = static_cast<float>(instance.random.below(std::max(1u, model.sequences[0].duration)));
            instance.variationTimer = instance.random.uniform(rendering::M2_VARIATION_TIMER_MIN_MS, rendering::M2_VARIATION_TIMER_MAX_MS);
        }

    auto seedIt = scene_->boneSeedInstanceByModel_.find(modelId);
    if (seedIt != scene_->boneSeedInstanceByModel_.end()) {
        auto idxIt = scene_->instanceIndexById.find(seedIt->second);
        if (idxIt != scene_->instanceIndexById.end() && idxIt->second < scene_->instances.size()) {
            const auto& existing = scene_->instances[idxIt->second];
            if (existing.modelId == modelId && !existing.boneMatrices.empty()) {
                instance.boneMatrices = existing.boneMatrices;
            } else {
                scene_->boneSeedInstanceByModel_.erase(seedIt);  // stale entry
            }
        } else {
            scene_->boneSeedInstanceByModel_.erase(seedIt);  // that instance is gone
        }
    }

    // No sibling to copy from, so pay for the bones now.
    if (instance.boneMatrices.empty()) {
        computeBoneMatrices(model, instance, &cachedView_);
    }
    if (!instance.boneMatrices.empty()) {
        scene_->boneSeedInstanceByModel_.emplace(modelId, instance.id);
    }
}

// A model the bone loop does not step still plays its first sequence: its
// texture scroll and its emitters are keyed to it, and update() wraps the
// clock at this length. Left at zero, the length was unknown, the clock was
// wrapped at a fixed 3.3 s instead, and a waterfall whose scroll lasts 2.6 s
// stood still for the difference every lap.
void M2Renderer::seedUnanimatedSequence(const M2ModelGPU& model, M2Instance& instance) {
    if (model.sequences.empty() || model.sequences[0].duration == 0) return;
    instance.currentSequenceIndex = 0;
    instance.idleSequenceIndex = 0;
    instance.animDuration = static_cast<float>(model.sequences[0].duration);
    instance.animTime = static_cast<float>(instance.random.below(model.sequences[0].duration));
}

uint32_t M2Renderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                     const glm::vec3& rotation, float scale,
                                     bool allowPositionDedup, uint32_t reservedId) {
    if (reservedId != 0) allowPositionDedup = false;
    // Reject NaN inputs at the boundary - std::round of NaN is implementation-
    // defined and a NaN instance position propagates into the GPU model matrix,
    // either tripping Vulkan validation or rendering at the world origin.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z) ||
        !std::isfinite(scale) || scale <= 0.0f) {
        return 0;
    }
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    const auto& mdlRef = (*modelIt->second);
    modelUnusedSince_.erase(modelId);


    // Deduplicate: skip if same model already at nearly the same position.
    // Uses hash map for O(1) lookup instead of O(N) scan.
    // Spell effects are exempt - transient visuals must always create fresh instances.
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = scene_->instanceDedupMap_.find(dk);
        if (dit != scene_->instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = reservedId != 0 ? reservedId : nextInstanceId++;
    instance.random = InstanceRandom(instance.id);
    instance.modelId = modelId;
    instance.position = position;
    if (mdlRef.isGroundDetail) {
        instance.position.z -= computeGroundDetailDownOffset(mdlRef, scale);
    }
    instance.rotation = rotation;
    instance.scale = scale;
    instance.updateModelMatrix();
    glm::vec3 localMin, localMax;
    collisionLocalBounds(mdlRef, localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);

    // Cache model flags on instance to avoid per-frame hash lookups
    instance.cachedHasAnimation = mdlRef.hasAnimation;
    instance.cachedDisableAnimation = mdlRef.disableAnimation;
    instance.cachedHasEmitters = mdlRef.hasEmitters();
    instance.cachedBoundRadius = mdlRef.boundRadius;
    instance.cachedIsGroundDetail = mdlRef.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdlRef.isInvisibleTrap;
    instance.cachedIsSkyBird = mdlRef.isSkyBird;
    instance.cachedIsLightBeam = mdlRef.isLightBeam;
    instance.cachedIsTransportDoodad = mdlRef.isTransportDoodad;
    instance.cachedIsBoat = mdlRef.isBoat;
    instance.cachedIsValid = mdlRef.isValid();
    instance.cachedModel = &mdlRef;
    instance.recomputeCachedCullFactors();

    // Initialize animation: play first sequence (usually Stand/Idle)
    const auto& mdl = mdlRef;
    if (mdl.hasAnimation && !mdl.disableAnimation) {
        seedInstanceAnimation(mdlRef, modelId, instance);
    } else {
        seedUnanimatedSequence(mdlRef, instance);
    }

    // Register in dedup map before pushing (uses original position, not ground-adjusted)
    // Spell effects are exempt from dedup tracking (transient, overlapping allowed).
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        scene_->instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    scene_->instances.push_back(std::move(instance));
    const size_t idx = scene_->instances.size() - 1;
    const M2Instance& placed = scene_->instances[idx];
    // Track special instances for fast-path iteration
    fileInIndexSets(idx);
    scene_->instanceIndexById[placed.id] = idx;
    scene_->rtChanges.add(placed.id, placed.modelId);
    fileInCullGrid(scene_->instances[idx]);
    placeInCollision(placed);

    return placed.id;
}

uint32_t M2Renderer::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                                const glm::vec3& position, uint32_t reservedId) {
    // Reject NaN inputs at the boundary. position feeds the dedup hash
    // (std::round of NaN is implementation-defined); the matrix goes
    // straight to the GPU UBO and would crash validation.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return 0;
    }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(modelMatrix[c][r])) return 0;
    if (models.find(modelId) == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    modelUnusedSince_.erase(modelId);

    // Deduplicate: O(1) hash lookup. Not an instance with an id of its own
    // (see createInstance).
    if (reservedId == 0) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = scene_->instanceDedupMap_.find(dk);
        if (dit != scene_->instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = reservedId != 0 ? reservedId : nextInstanceId++;
    instance.random = InstanceRandom(instance.id);
    instance.modelId = modelId;
    instance.position = position;  // Used for frustum culling
    instance.rotation = glm::vec3(0.0f);
    instance.scale = 1.0f;
    instance.modelMatrix = modelMatrix;
    instance.invModelMatrix = glm::inverse(modelMatrix);
    glm::vec3 localMin, localMax;
    collisionLocalBounds(*models[modelId], localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);
    // Cache model flags on instance to avoid per-frame hash lookups
    const auto& mdl2 = *models[modelId];
    instance.cachedHasAnimation = mdl2.hasAnimation;
    instance.cachedDisableAnimation = mdl2.disableAnimation;
    instance.cachedHasEmitters = mdl2.hasEmitters();
    instance.cachedBoundRadius = mdl2.boundRadius;
    instance.cachedIsGroundDetail = mdl2.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdl2.isInvisibleTrap;
    instance.cachedIsSkyBird = mdl2.isSkyBird;
    instance.cachedIsLightBeam = mdl2.isLightBeam;
    instance.cachedIsTransportDoodad = mdl2.isTransportDoodad;
    instance.cachedIsBoat = mdl2.isBoat;
    instance.cachedIsValid = mdl2.isValid();
    instance.cachedModel = &mdl2;
    instance.recomputeCachedCullFactors();

    // Initialize animation
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        seedInstanceAnimation(mdl2, modelId, instance);
    } else {
        // A model with no skeleton can still have particle emitters, and their
        // rate and lifespan tracks are sampled at animTime. Starting every
        // instance at zero puts a courtyard of identical torches in lockstep,
        // so the phase is spread.
        //
        // createInstance above now does the same.
        instance.animTime = instance.random.uniform(0.0f, 10000.0f);
        seedUnanimatedSequence(mdl2, instance);
    }

    // Register in dedup map
    if (reservedId == 0) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        scene_->instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    scene_->instances.push_back(std::move(instance));
    const size_t idx = scene_->instances.size() - 1;
    const M2Instance& placed = scene_->instances[idx];
    fileInIndexSets(idx);
    scene_->instanceIndexById[placed.id] = idx;
    scene_->rtChanges.add(placed.id, placed.modelId);
    fileInCullGrid(scene_->instances[idx]);
    placeInCollision(placed);

    return placed.id;
}

// WOWEE_SKY_M2_MAX_BATCH=<n> draws only the sky model's first n layers.
//
// Thirty-four of them, and every measurement says the set drawn is the same
// every frame - so if the flicker is one layer's doing, bisecting the count
// finds it in about five runs. That has worked three times on this fault where
// reading the code has worked none.
static bool skyBatchAllowed(bool skyMode, std::size_t index) {
    if (!skyMode) return true;
    static const int maxBatch = [] {
        const char* set = std::getenv("WOWEE_SKY_M2_MAX_BATCH");
        return set ? std::atoi(set) : -1;
    }();
    return maxBatch < 0 || static_cast<int>(index) < maxBatch;
}

void M2Renderer::update(float deltaTime, const glm::mat4& view, const glm::mat4& projection) {
    const glm::vec3 cameraPos = glm::vec3(glm::inverse(view)[3]);
    const glm::mat4 viewProjection = projection * view;
    cachedView_ = view;
    ZoneScopedN("M2Renderer::update");
    // The spatial grid, the cull grid and the id map are kept up to date by
    // every create, move and removal on the main thread; this update, which
    // runs on a frame worker, only reads them.

    float dtMs = deltaTime * 1000.0f;

    // Cache camera state for frustum-culling bone computation
    cachedCamPos_ = cameraPos;
    // Never past the ground. The density constants are how far models are
    // worth drawing, not how far there is anything to draw them on: the
    // terrain and the WMOs stop at the view distance itself, so a doodad
    // beyond it is a tree standing on nothing.
    const float maxRenderDistance = std::min(
        cappedViewDistance(),
        viewDistanceScale_ *
            ((scene_->instances.size() > rendering::M2_HIGH_DENSITY_INSTANCE_THRESHOLD)
                 ? rendering::M2_MAX_RENDER_DISTANCE_HIGH_DENSITY
                 : rendering::M2_MAX_RENDER_DISTANCE_LOW_DENSITY));
    cachedMaxRenderDistSq_ = maxRenderDistance * maxRenderDistance;

    cachedViewProjection_ = viewProjection;

    // Build frustum for culling bones
    Frustum updateFrustum;
    updateFrustum.extractFromMatrix(viewProjection);

    // --- Normal M2 animation update ---
    // Advance animTime for ALL instances (needed for texture UV animation on static doodads).
    // This is a tight loop touching only one float per instance - no hash lookups.
    //
    // An instance the bone loop below does not step is wrapped here, at the
    // length of its own sequence. A texture track is sampled against this
    // time and holds its last key once the time runs past the end, so a
    // waterfall with no bones of its own scrolled once after it loaded and
    // then stood still, and one with emitters - wrapped at a fixed 3.3 s
    // below, whatever its sequence - stopped for the rest of each lap.
    for (auto& instance : scene_->instances) {
        // A hidden instance's clock stops, so it resumes where it was shown.
        if (instance.hidden) continue;
        instance.animTime += dtMs;
        instance.globalSequenceTime += dtMs;
        // A boat asks once whether it is on water - the canal may stream in
        // after it does, so it keeps asking for a while - and rides it if so.
        // A rowboat pulled up on a beach is the same model and stays put.
        if (instance.cachedIsBoat && instance.afloat < 0 && waterHeightAt_) {
            instance.afloatRecheck -= deltaTime;
            if (instance.afloatRecheck <= 0.0f) {
                instance.afloatRecheck = 1.0f;
                const auto water = waterHeightAt_(instance.position.x, instance.position.y,
                                                  instance.position.z);
                if (water && std::abs(*water - instance.position.z) < 1.5f) {
                    instance.afloat = 1;
                } else if (++instance.afloatTries >= 15) {
                    instance.afloat = 0;
                }
            }
        }
        const bool steppedBelow = instance.cachedHasAnimation && !instance.cachedDisableAnimation;
        if (!steppedBelow && instance.animDuration > 0.0f &&
            instance.animTime >= instance.animDuration) {
            instance.animTime = std::fmod(instance.animTime, instance.animDuration);
        }
    }

    // The sky model's clock, when this is the renderer that draws one.
    //
    // A report of the sky "playing an animation that brightens and dims, faster
    // when walking" survived three fixes to what chooses the sky, and the
    // lighting diagnostic then showed every input to it holding still while it
    // happened - the zone, the volumes, the model and the target colour. So
    // what is moving is this, and the two things worth telling apart are
    // whether the instance is being rebuilt (animTime back to zero) and
    // whether the clock runs at wall speed (animTime should advance by dtMs and
    // by nothing else).
    //
    // Only this renderer has skyMode_, and only on a change worth seeing, so it
    // is quiet unless asked for with WOWEE_LOG_LEVEL=info.
    if (skyMode_ && !scene_->instances.empty()) {
        const auto& sky = scene_->instances.front();
        const bool restarted = sky.animTime < skyDiagAnimTime_;
        if (restarted || sky.id != skyDiagInstanceId_ ||
            sky.animTime - skyDiagAnimTime_ > 1000.0f) {
            LOG_INFO("skyM2: instance=", sky.id, " instances=", scene_->instances.size(),
                     " animTime=", sky.animTime, " duration=", sky.animDuration,
                     " gsTime=", sky.globalSequenceTime, " dtMs=", dtMs,
                     restarted ? " RESTARTED" : "");
            skyDiagInstanceId_ = sky.id;
            skyDiagAnimTime_ = sky.animTime;
        }
    }
    // Wrap animTime for particle-only instances so emission rate tracks keep looping.
    // 3333ms chosen as a safe wrap period: long enough to cover the longest known M2
    // particle emission cycle (~3s for torch/campfire effects) while preventing float
    // precision loss that accumulates over hours of runtime.
    static constexpr float kParticleWrapMs = 3333.0f;
    // Only for those with no sequence length to wrap at; the rest were
    // wrapped at their own above.
    for (size_t idx : scene_->particleOnlyInstanceIndices_) {
        if (idx >= scene_->instances.size()) continue;
        auto& instance = scene_->instances[idx];
        if (instance.animDuration > 0.0f) continue;
        // Use iterative subtraction instead of fmod() to preserve precision
        while (instance.animTime > kParticleWrapMs) {
            instance.animTime -= kParticleWrapMs;
        }
    }

    scene_->boneWorkIndices_.clear();
    scene_->boneWorkIndices_.reserve(scene_->animatedInstanceIndices_.size());

    // Update animated instances (full animation state + bone computation culling)
    // Note: animTime was already advanced by dtMs in the global loop above.
    // Here we apply the speed factor: subtract the base dtMs and add dtMs*speed.
    // Ground clutter stops being stepped once it is past the distance it draws
    // at. There are hundreds of tufts to a tile and each one plays a sequence of
    // its own, so this list is mostly grass that nothing can see; the sequences
    // loop, so one that resumes from a stale time is indistinguishable from one
    // that never stopped.
    const float clutterAnimCutoffSq = (groundDetailMaxDistance_ > 0.0f)
        ? (groundDetailMaxDistance_ * groundDetailMaxDistance_) : 0.0f;

    for (size_t idx : scene_->animatedInstanceIndices_) {
        if (idx >= scene_->instances.size()) continue;
        auto& instance = scene_->instances[idx];

        if (clutterAnimCutoffSq > 0.0f && instance.cachedIsGroundDetail) {
            const glm::vec3 toCam = instance.position - cachedCamPos_;
            if (glm::dot(toCam, toCam) > clutterAnimCutoffSq) continue;
        }

        instance.animTime += dtMs * (instance.animSpeed - 1.0f);

        // For animation looping/variation, we need the actual model data.
        if (!instance.cachedModel) continue;
        const M2ModelGPU& model = *instance.cachedModel;

        // Validate sequence index
        if (instance.currentSequenceIndex < 0 ||
            instance.currentSequenceIndex >= static_cast<int>(model.sequences.size())) {
            instance.currentSequenceIndex = 0;
            if (!model.sequences.empty()) {
                instance.animDuration = static_cast<float>(model.sequences[0].duration);
            }
        }

        // Handle animation looping / variation transitions
        if (instance.animDuration <= 0.0f && instance.cachedHasEmitters) {
            instance.animDuration = rendering::M2_DEFAULT_PARTICLE_ANIM_MS;
        }
        // Played backwards (a GameObject's Close standing in for a missing
        // Open), a one-shot ends at its start.
        if (instance.animSpeed < 0.0f && instance.animTime <= 0.0f) {
            instance.animTime = 0.0f;
            if (instance.nextAnimation) {
                advanceToNextAnimation(instance);
            } else {
                instance.animSpeed = 0.0f;
            }
        }
        if (instance.animDuration > 0.0f && instance.animTime >= instance.animDuration &&
            instance.nextAnimation) {
            advanceToNextAnimation(instance);
        } else if (instance.animDuration > 0.0f && instance.animTime >= instance.animDuration) {
            if (instance.holdAtEnd) {
                // Stay on the last frame. A door's open sequence ends with the
                // door open, and that is the pose the server is describing.
                instance.animTime = instance.animDuration;
                instance.animSpeed = 0.0f;
            } else if (instance.playingVariation) {
                instance.playingVariation = false;
                instance.currentSequenceIndex = instance.idleSequenceIndex;
                if (instance.idleSequenceIndex < static_cast<int>(model.sequences.size())) {
                    instance.animDuration = static_cast<float>(model.sequences[instance.idleSequenceIndex].duration);
                }
                instance.animTime = 0.0f;
                instance.variationTimer = instance.random.uniform(rendering::M2_LOOP_VARIATION_TIMER_MIN_MS, rendering::M2_LOOP_VARIATION_TIMER_MAX_MS);
            } else {
                // Use iterative subtraction instead of fmod() to preserve precision
                float duration = std::max(1.0f, instance.animDuration);
                while (instance.animTime >= duration) {
                    instance.animTime -= duration;
                }
            }
        }

        // Idle variation timer
        if (!instance.playingVariation && !instance.holdAtEnd &&
            model.idleVariationIndices.size() > 1) {
            instance.variationTimer -= dtMs;
            if (instance.variationTimer <= 0.0f) {
                int pick = static_cast<int>(instance.random.below(static_cast<uint32_t>(model.idleVariationIndices.size())));
                int newSeq = model.idleVariationIndices[pick];
                if (newSeq != instance.currentSequenceIndex && newSeq < static_cast<int>(model.sequences.size())) {
                    instance.playingVariation = true;
                    instance.currentSequenceIndex = newSeq;
                    instance.animDuration = static_cast<float>(model.sequences[newSeq].duration);
                    instance.animTime = 0.0f;
                } else {
                    instance.variationTimer = instance.random.uniform(rendering::M2_IDLE_VARIATION_TIMER_MIN_MS, rendering::M2_IDLE_VARIATION_TIMER_MAX_MS);
                }
            }
        }

        // Frustum + distance cull: skip expensive bone computation for off-screen instances.
        // Both effectiveMaxDistSq and paddedRadius are precomputed per instance in
        // recomputeCachedCullFactors(); we only need the per-frame distance and frustum test.
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        float effectiveMaxDistSq = rendering::m2InstanceMaxDistSq(
            cachedMaxRenderDistSq_, instance.cachedEffectiveMaxDistSqFactor,
            false, 0.0f, cappedViewDistance(),
            instance.cachedIsGroundDetail, groundDetailMaxDistance_);
        if (instance.cachedIsSkyBird) {
            constexpr float kBirdMaxDistSq =
                rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
            effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
        }
        if (distSq > effectiveMaxDistSq) continue;
        float paddedRadius = instance.cachedPaddedRadius;
        if (paddedRadius > 0.0f && !updateFrustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;

        // LOD 3 skip: models beyond 150 units use the lowest LOD mesh which has
        // no visible skeletal animation.  Keep their last-computed bone matrices
        // (always valid - seeded on spawn) and avoid the expensive per-bone work.
        // Sky birds, light beams, and ship machinery are exempt: their visible
        // motion is baked entirely into bone animation.
        constexpr float kLOD3DistSq = rendering::M2_LOD3_DISTANCE * rendering::M2_LOD3_DISTANCE;
        const bool needsDistantBones = instance.cachedIsSkyBird || instance.cachedIsLightBeam ||
                                       instance.cachedIsTransportDoodad;
        if (distSq > kLOD3DistSq && !needsDistantBones) continue;

        // Distance-based frame skipping: update distant bones less frequently
        uint32_t boneInterval = 1;
        if (!needsDistantBones) {
            if (distSq > rendering::M2_BONE_SKIP_DIST_FAR * rendering::M2_BONE_SKIP_DIST_FAR) boneInterval = 4;
            else if (distSq > rendering::M2_BONE_SKIP_DIST_MID * rendering::M2_BONE_SKIP_DIST_MID) boneInterval = 2;
        }
        instance.frameSkipCounter++;
        if ((instance.frameSkipCounter % boneInterval) != 0) continue;

        scene_->boneWorkIndices_.push_back(idx);
    }

    // Compute bone matrices (expensive, parallel if enough work)
    const size_t animCount = scene_->boneWorkIndices_.size();
    if (animCount > 0) {
        static const size_t minParallelAnimInstances = std::max<size_t>(
            8, envSizeOrDefault("WOWEE_M2_ANIM_MT_MIN", 96));
        if (animCount < minParallelAnimInstances || numAnimThreads_ <= 1) {
            // Sequential - not enough work to justify thread overhead
            for (size_t i : scene_->boneWorkIndices_) {
                if (i >= scene_->instances.size()) continue;
                auto& inst = scene_->instances[i];
                if (!inst.cachedModel) continue;
                computeBoneMatrices(*inst.cachedModel, inst, &cachedView_);
            }
        } else {
            // Parallel - dispatch across worker threads
            static const size_t minAnimWorkPerThread = std::max<size_t>(
                16, envSizeOrDefault("WOWEE_M2_ANIM_WORK_PER_THREAD", 64));
            const size_t maxUsefulThreads = std::max<size_t>(
                1, (animCount + minAnimWorkPerThread - 1) / minAnimWorkPerThread);
            const size_t numThreads = std::min(static_cast<size_t>(numAnimThreads_), maxUsefulThreads);
            if (numThreads <= 1) {
                for (size_t i : scene_->boneWorkIndices_) {
                    if (i >= scene_->instances.size()) continue;
                    auto& inst = scene_->instances[i];
                    if (!inst.cachedModel) continue;
                    computeBoneMatrices(*inst.cachedModel, inst, &cachedView_);
                }
            } else {
                const size_t chunkSize = animCount / numThreads;
                const size_t remainder = animCount % numThreads;

                auto processRange = [this](size_t begin, size_t end) {
                    for (size_t j = begin; j < end; ++j) {
                        size_t idx = scene_->boneWorkIndices_[j];
                        if (idx >= scene_->instances.size()) continue;
                        auto& inst = scene_->instances[idx];
                        if (!inst.cachedModel) continue;
                        computeBoneMatrices(*inst.cachedModel, inst, &cachedView_);
                    }
                };

                // Reuse persistent futures vector to avoid allocation
                scene_->animFutures_.clear();
                if (scene_->animFutures_.capacity() < numThreads) {
                    scene_->animFutures_.reserve(numThreads);
                }

                // Dispatch all but the last chunk to the shared pool; process the
                // last chunk on this thread so this call always makes progress even
                // when it itself runs on a pool worker (see ThreadPool docs).
                size_t start = 0;
                for (size_t t = 0; t + 1 < numThreads; ++t) {
                    size_t end = start + chunkSize + (t < remainder ? 1 : 0);
                    scene_->animFutures_.push_back(core::ThreadPool::frameWorkers().submit(
                        "M2 animation: chunk",
                        [processRange, start, end]() { processRange(start, end); }));
                    start = end;
                }
                processRange(start, animCount);

                for (auto& f : scene_->animFutures_) {
                    f.get();
                }
            }
        }
    }

    // WOWEE_M2_PARTICLE_DIAG=<part of a model path>: every two seconds, for
    // each instance of a matching model, whether its particles are being
    // flown and where they are. A model that shows its particles in the
    // model probe and none in the world is told apart here: skipped for
    // distance, emitting nothing, or emitting somewhere else.
    static const std::string kParticleDiag = [] {
        const char* v = std::getenv("WOWEE_M2_PARTICLE_DIAG");
        std::string s = v ? v : "";
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    static float particleDiagClock = 0.0f;
    bool particleDiagNow = false;
    if (!kParticleDiag.empty()) {
        particleDiagClock += deltaTime;
        if (particleDiagClock >= 2.0f) {
            particleDiagClock = 0.0f;
            particleDiagNow = true;
        }
    }

    // Particle update (sequential - uses RNG, not thread-safe)
    // Only iterate instances that have particle emitters (pre-built list).
    for (size_t idx : scene_->particleInstanceIndices_) {
        if (idx >= scene_->instances.size()) continue;
        auto& instance = scene_->instances[idx];
        // Distance cull: only update particles within visible range
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        if (particleDiagNow && instance.cachedModel) {
            std::string name = instance.cachedModel->name;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.find(kParticleDiag) != std::string::npos) {
                glm::vec3 offset(0.0f);
                if (!instance.particles.empty()) offset = instance.particles.front().position - instance.position;
                LOG_WARNING("M2 PARTICLES '", instance.cachedModel->name, "' instance ", instance.id,
                            " gameObject=", instance.isGameObject ? 1 : 0,
                            " dist=", std::sqrt(distSq), " max=", std::sqrt(cachedMaxRenderDistSq_),
                            " emitters=", instance.cachedModel->particleEmitters.size(),
                            " live=", instance.particles.size(),
                            " first at (", offset.x, ", ", offset.y, ", ", offset.z, ") from the instance",
                            " bones=", instance.boneMatrices.size(),
                            " seq=", instance.currentSequenceIndex, " animTime=", instance.animTime,
                            " scale=", instance.scale,
                            distSq > cachedMaxRenderDistSq_ ? "  SKIPPED: beyond render distance" : "");
            }
        }
        if (distSq > cachedMaxRenderDistSq_) continue;
        if (!instance.cachedModel) continue;
        // Out of view, a model is not animated and its particles freeze, as
        // in the client (SetAnimating, 0x823f10) - unless it is near: within
        // 33.3 yards for an entity (a game object, or a spell effect on a
        // unit; 0x793450), within 10 for a doodad (0x7987a0). The sphere
        // takes in how far its particles have gone.
        const bool entity = instance.isGameObject || instance.cachedModel->isSpellEffect;
        const float animateUnseenWithinSq = entity ? 1111.1f : 100.0f;
        constexpr float kParticleSizeMargin = 2.0f;
        const float viewRadius = std::max(instance.cachedPaddedRadius, instance.particleReach) +
                                 kParticleSizeMargin;
        if (distSq > animateUnseenWithinSq &&
            !updateFrustum.intersectsSphere(instance.cachedCullCenter, viewRadius)) {
            continue;
        }
        // A long frame in steps of at most 0.1 s, and no more than the
        // longest lifespan's worth of them (0x97acb0).
        const M2ModelGPU& model = *instance.cachedModel;
        float longestLife = 0.0f;
        for (const auto& em : model.particleEmitters) {
            longestLife = std::max(longestLife,
                interpFloat(em.lifespan, instance.animTime, instance.globalSequenceTime,
                            instance.currentSequenceIndex, model.globalSequenceDurations) +
                em.lifespanVary);
        }
        float remaining = longestLife > 0.0f ? std::min(deltaTime, longestLife) : deltaTime;
        while (remaining > 0.0f) {
            const float step = std::min(remaining, kM2ParticleMaxStep);
            remaining -= step;
            // A finished effect emits nothing more; what it has flies on.
            if (!instance.finishing) emitParticles(instance, model, step);
            updateParticles(instance, step);
        }
        if (!model.ribbonEmitters.empty()) {
            updateRibbons(instance, model, deltaTime, !instance.finishing);
        }
    }

}

// Diagnostic: WOWEE_M2_NO_SKINNING=1 renders every M2 in its bind pose by
// telling the shader to ignore bones, separating a skinning artifact from one
// drawn by the particle or ribbon systems.
static const bool kM2NoSkinning = envFlagEnabled("WOWEE_M2_NO_SKINNING");

void M2Renderer::extractBones(Frame& frame) {
    if (!initialized_ || scene_->instances.empty()) return;
    reapFinishedInstances();

    // --- Mega bone SSBO: assign ranges and upload all animated instance bones ---
    // Offset 0 is reserved as the identity/no-bones sentinel; animated instances
    // are packed after it at their own bone count, so a 300-bone creature gets
    // all 300 matrices instead of being truncated into a fixed stride and
    // reading into its neighbour's range.
    uint32_t nextOffset = 1;
    frame.bones.emplace_back(1.0f);
    for (size_t idx : scene_->animatedInstanceIndices_) {
        if (idx >= scene_->instances.size()) continue;
        auto& instance = scene_->instances[idx];

        if (instance.boneMatrices.empty()) {
            instance.megaBoneOffset = 0;  // Use identity slot
            continue;
        }

        // A bone that has left the model behind.
        //
        // The vertex shader skins by these matrices, so one that translates far
        // outside the model's own bounds drags every vertex weighted to it out
        // with it. What that looks like on screen is not a bone problem: it is
        // a long flat triangle with the skin smeared across it, which reads as
        // a missing texture or a stray card. The Elemental Slave's white sheets
        // were reported three times as missing textures, and the texture paths,
        // the display skins, the particle emitters and the ribbons were all
        // measured and found correct before the geometry was suspected.
        //
        // Judged against the other bones, not against the model's origin, and
        // against the model's own bounding radius so a large creature is not
        // accused for being large. Measuring from the origin flags every
        // flying critter: a bird's flight path is authored into its root bone,
        // so the whole skeleton is sixteen yards out and nothing is stretched.
        // What a stretch looks like is one bone leaving the set behind.
        if (instance.cachedModel != nullptr) {
            const M2ModelGPU& m = *instance.cachedModel;
            const glm::vec3 extent = m.boundMax - m.boundMin;
            const float radius = 0.5f * glm::length(extent);
            // Examined once per model, not once per frame: this walks every
            // bone, and there are models with three hundred of them drawn
            // dozens at a time.
            static std::unordered_set<uint32_t> boneRangeChecked;
            if (radius > 0.01f && boneRangeChecked.size() < 64 &&
                boneRangeChecked.insert(instance.modelId).second) {
                glm::vec3 centre(0.0f);
                for (const auto& bone : instance.boneMatrices) centre += glm::vec3(bone[3]);
                centre /= static_cast<float>(instance.boneMatrices.size());

                const float limit = radius * 4.0f;
                for (size_t bi = 0; bi < instance.boneMatrices.size(); ++bi) {
                    const glm::vec3 t(instance.boneMatrices[bi][3]);
                    const float stray = glm::length(t - centre);
                    if (stray <= limit) continue;
                    LOG_WARNING("M2 '", m.name, "' bone ", bi, " of ",
                                instance.boneMatrices.size(), " sits ", stray,
                                " from where the rest of its skeleton is, in a model whose"
                                " bounding radius is ", radius,
                                " - anything weighted to it is drawn stretched");
                    break;
                }
            }
        }

        const uint32_t boneCount = static_cast<uint32_t>(instance.boneMatrices.size());
        if (boneCount > MEGA_BONE_MATRIX_CAPACITY - nextOffset) {
            instance.megaBoneOffset = 0;  // Overflow - use identity
            continue;
        }

        instance.megaBoneOffset = nextOffset;

        frame.bones.insert(frame.bones.end(), instance.boneMatrices.begin(), instance.boneMatrices.end());

        nextOffset += boneCount;
    }
}

bool M2Renderer::prepareInstanceStorage(uint32_t slot,uint64_t entries) {
    if(entries<=instanceCapacity_[slot]) return true;
    if(instanceGrowthFailed_[slot]) return false;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vkCtx_->getPhysicalDevice(),&properties);
    const uint64_t limit=properties.limits.maxStorageBufferRange/sizeof(M2InstanceGPU);
    const auto fail=[&](const char* why) {
        instanceGrowthFailed_[slot]=true;
        LOG_ERROR("M2 instance storage: need ",entries," records in slot ",slot,
            ", capacity ",instanceCapacity_[slot],", device limit ",limit,"; ",why,
            " - M2 meshes omitted while this frame exceeds available storage");
        return false;
    };
    if(entries>limit) return fail("storage-buffer range exceeded");
    const uint32_t capacity=static_cast<uint32_t>(std::min(limit,
        std::max(entries,uint64_t(instanceCapacity_[slot])*2)));
    const VkDeviceSize bytes=VkDeviceSize(capacity)*sizeof(M2InstanceGPU);
    VkBufferCreateInfo bci{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size=bytes; bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage=VMA_MEMORY_USAGE_CPU_TO_GPU;
    aci.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT;
    AllocatedBuffer next;
    if(gpuCreateBuffer(vkCtx_->getAllocator(),&bci,&aci,&next.buffer,&next.allocation,&next.info)!=VK_SUCCESS)
        return fail("mapped GPU allocation failed");
    if(!next.info.pMappedData) {
        gpuDestroyBuffer(vkCtx_->getAllocator(),next.buffer,next.allocation);
        return fail("GPU allocation has no CPU mapping");
    }
    // prepareRender runs after this slot's fence, before any M2 command is
    // recorded. The other slot's descriptor and buffer remain untouched.
    VkDescriptorBufferInfo info{next.buffer,0,bytes};
    VkWriteDescriptorSet write{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=instanceSet_[slot]; write.dstBinding=0; write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&info;
    vkUpdateDescriptorSets(vkCtx_->getDevice(),1,&write,0,nullptr);
    DeferredResources retire(*vkCtx_);
    retire.buffer(instanceBuffer_[slot],instanceAlloc_[slot]);
    instanceBuffer_[slot]=next.buffer; instanceAlloc_[slot]=next.allocation;
    instanceMapped_[slot]=next.info.pMappedData; instanceCapacity_[slot]=capacity;
    LOG_INFO("M2 instance storage: slot ",slot," grew to ",capacity," records (",bytes/1024,
        " KiB), frame requires ",entries);
    return true;
}

void M2Renderer::prepareRender(uint32_t frameIndex, const Camera&) {
    if(!prepareInstanceStorage(frameIndex,recording_->instanceEntries)) return;
    if (megaBoneMapped_[frameIndex] && !recording_->bones.empty())
        std::memcpy(megaBoneMapped_[frameIndex], recording_->bones.data(), recording_->bones.size() * sizeof(glm::mat4));
}


// Dispatch GPU frustum culling compute shader into the primary frame command
// buffer. render() consumes the completed output left in this frame slot from
// its previous use; this dispatch produces results for the slot's next reuse.
void M2Renderer::extractCull(const Camera& camera, Frame& frame) {


    // --- Compute per-instance adaptive distances (same formula as old CPU cull) ---
    const float targetRenderDist = viewDistanceScale_ *
        ((scene_->instances.size() > 2000) ? 300.0f
         : (scene_->instances.size() > 1000) ? 500.0f
                                     : 1000.0f);
    const float shrinkRate = 0.005f;
    const float growRate   = 0.05f;
    float blendRate = (targetRenderDist < smoothedRenderDist_) ? shrinkRate : growRate;
    smoothedRenderDist_ = glm::mix(smoothedRenderDist_, targetRenderDist, blendRate);
    const float maxRenderDistance = smoothedRenderDist_;
    const float maxRenderDistanceSq = maxRenderDistance * maxRenderDistance;
    // Every per-instance range is capped by View Distance. The broad phase
    // must cover that ceiling: twice the adaptive clutter range can be much
    // shorter than a large landmark's own range once a dense scene settles.
    const float maxPossibleDistSq = cappedViewDistance() * cappedViewDistance();

    // --- Upload frustum planes + camera (UBO, binding 0) ---
    const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
    Frustum frustum;
    frustum.extractFromMatrix(vp);
    const glm::vec3 camPos = camera.getPosition();

    // Only the instances in columns this camera may see go to the GPU. This
    // took the first MAX_CULL_INSTANCES of the array whatever they were - in a
    // city with ninety thousand resident that was a quarter of them, picked by
    // load order, with the rest left to a CPU test in render().
    gatherViewCandidates(camPos, frustum, maxPossibleDistSq, cullCandidates_);
    const uint32_t numInstances =
        std::min(static_cast<uint32_t>(cullCandidates_.size()), MAX_CULL_INSTANCES);

    frame.maxPossibleDistSq = maxPossibleDistSq;
    frame.cullIds.reserve(numInstances);
    frame.cullInput.reserve(numInstances);
    for (uint32_t i = 0; i < numInstances; ++i) {
        const auto& inst = scene_->instances[cullCandidates_[i]];
        float distanceSq = rendering::m2InstanceMaxDistSq(maxRenderDistanceSq,
            inst.cachedEffectiveMaxDistSqFactor, inst.isGameObject,
            rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE, cappedViewDistance(),
            inst.cachedIsGroundDetail, groundDetailMaxDistance_);
        if (inst.cachedIsSkyBird && inst.cachedHasAnimation && !inst.cachedDisableAnimation)
            distanceSq = std::min(distanceSq, rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE * rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE);
        frame.cullIds.push_back(inst.id);
        frame.cullInput.push_back({glm::vec4(inst.cachedCullCenter, inst.cachedPaddedRadius), distanceSq,
                                   (inst.cachedIsValid ? 1u : 0u) | (inst.cachedIsInvisibleTrap ? 4u : 0u) |
                                   (inst.hizPrevCulledFrames < 2 && !inst.isGameObject ? 8u : 0u), {0.0f, 0.0f}});
    }
}

void M2Renderer::dispatchCullCompute(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera) {
    if (!cullPipeline_ || recording_->cullInput.empty()) return;
    const uint32_t numInstances = static_cast<uint32_t>(recording_->cullInput.size());
    const float maxPossibleDistSq = recording_->maxPossibleDistSq;
    const glm::mat4 vp = camera.getViewProjectionMatrix();
    Frustum frustum;
    frustum.extractFromMatrix(vp);
    const glm::vec3 camPos = camera.getPosition();
    if (cullUniformMapped_[frameIndex]) {
        auto* ubo = static_cast<CullUniformsGPU*>(cullUniformMapped_[frameIndex]);
        for (int i = 0; i < 6; i++) {
            const auto& p = frustum.getPlane(static_cast<Frustum::Side>(i));
            ubo->frustumPlanes[i] = glm::vec4(p.normal, p.distance);
        }
        ubo->cameraPos = glm::vec4(camPos, maxPossibleDistSq);
        ubo->instanceCount = numInstances;

        // HiZ occlusion culling fields
        const bool hizReady = hizSystem_ && hizSystem_->isReady();

        // Auto-disable HiZ when the camera has moved/rotated significantly.
        // Large VP changes make the depth pyramid unreliable because the
        // reprojected screen positions diverge from the actual pyramid data.
        bool hizSafe = hizReady;
        if (hizReady) {
            // Compare current VP against previous VP - Frobenius-style max diff.
            float maxDiff = 0.0f;
            const float* curM  = &vp[0][0];
            const float* prevM = &prevVP_[0][0];
            for (int k = 0; k < 16; ++k)
                maxDiff = std::max(maxDiff, std::abs(curM[k] - prevM[k]));
            // Threshold: typical tracking-camera motion (following a walking
            // character) produces diffs of 0.05–0.25.  A fast rotation or
            // zoom easily exceeds 0.5.  The previous threshold (0.15) caused
            // the HiZ pass to toggle on/off every other frame during normal
            // gameplay, which produced global M2 doodad flicker.
            if (maxDiff > rendering::HIZ_VP_DIFF_THRESHOLD) hizSafe = false;
        }

        ubo->hizEnabled = hizSafe ? 1u : 0u;
        ubo->hizMipLevels = hizReady ? hizSystem_->getMipLevels() : 0u;
        ubo->_pad2 = 0;
        if (hizReady) {
            ubo->hizParams = glm::vec4(
                static_cast<float>(hizSystem_->getPyramidWidth()),
                static_cast<float>(hizSystem_->getPyramidHeight()),
                camera.getNearPlane(),
                0.0f
            );
            ubo->viewProj = vp;
            // Use previous frame's VP for HiZ reprojection - the HiZ pyramid
            // was built from the previous frame's depth, so we must project
            // into the same screen space to sample the correct depths.
            ubo->prevViewProj = prevVP_;
        } else {
            ubo->hizParams = glm::vec4(0.0f);
            ubo->viewProj = glm::mat4(1.0f);
            ubo->prevViewProj = glm::mat4(1.0f);
        }

        // Save current VP for next frame's temporal reprojection
        prevVP_ = vp;
    }

    collectCullOutput(frameIndex);
    submittedCullIds_[frameIndex] = recording_->cullIds;
    submittedCullScenes_[frameIndex] = recording_->sceneIdentity;
    submittedCullFrames_[frameIndex] = vkCtx_->frameNumber();

    // --- Upload per-instance cull data (SSBO, binding 1) ---
    // The per-instance radius math used to be recomputed here every frame; it's
    // now precomputed once by recomputeCachedCullFactors() since it depends only
    // on static instance state (scale, bound radius, animation/ground flags).
    if (cullInputMapped_[frameIndex] && numInstances)
        std::memcpy(cullInputMapped_[frameIndex], recording_->cullInput.data(), numInstances * sizeof(CullInstanceGPU));


    // --- Dispatch compute shader ---
    const bool useHiZ = (cullHiZPipeline_ != VK_NULL_HANDLE)
                     && hizSystem_ && hizSystem_->isReady();
    if (useHiZ) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullHiZPipeline_);
        // Set 0: cull UBO + input/output SSBOs
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cullHiZPipelineLayout_, 0, 1, &cullSet_[frameIndex], 0, nullptr);
        // Set 1: HiZ pyramid sampler
        VkDescriptorSet hizSet = hizSystem_->getDescriptorSet(frameIndex);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cullHiZPipelineLayout_, 1, 1, &hizSet, 0, nullptr);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cullPipelineLayout_, 0, 1, &cullSet_[frameIndex], 0, nullptr);
    }

    const uint32_t groupCount = (numInstances + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Make writes available to the host after this frame's fence signals. The
    // CPU invalidates and reads them when this frame slot is reused.
    VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_HOST_BIT;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    cmdPipelineBarrier2(cmd, dep);
}

namespace {

// Push constants now carry per-batch data only; per-instance data is in instance SSBO.
struct M2PushConstants {
    int32_t texCoordSet;        // UV set index (0 or 1)
    int32_t isFoliage;          // -1 = sky, 0 = none, 1 = wind foliage, 2 = ground clutter
    int32_t instanceDataOffset; // Base index into instance SSBO for this draw group
    float swayRefHeight;        // Model-space height the wind normalises against
    float swayAmp;              // Wind amplitude scale; 1.0 = the tree-sized default
    float plantHeight;          // The model's own height, for the player brush
    int32_t texCoordSet2;       // the second stage's: 0 UV0, 1 UV1, -1 sphere map, 2 UV0 unanimated
};
static_assert(sizeof(M2PushConstants) == kM2PushConstantBytes, "m2.vert's Push block");

// Where in its draw distance a doodad starts to fade out.
constexpr float kFadeStartFraction = 0.75f;

// The level of detail a model is drawn at this far away. Down to the nearest
// level the model actually has, not all the way back to full detail: a model
// carrying 0 and 1 and asked for 3 was drawn at 0 - the most expensive level
// there is - at the distance where it mattered least.
uint16_t m2LodFor(const M2ModelGPU& model, float distSq) {
    uint16_t lod = 0;
    if (distSq > 150.0f * 150.0f) lod = 3;
    else if (distSq > 80.0f * 80.0f) lod = 2;
    else if (distSq > 40.0f * 40.0f) lod = 1;
    while (lod > 0 && !(model.availableLODs & (1u << lod))) --lod;
    return lod;
}

// How far through its fade-out an instance this far away is: 1 until the
// fade starts, 0 at the end of its draw distance.
float m2DistanceFade(const M2ModelGPU& model, float distSq, float effectiveMaxDistSq) {
    const float fadeFrac = model.disableAnimation ? 0.55f : kFadeStartFraction;
    const float fadeStartDistSq = effectiveMaxDistSq * fadeFrac * fadeFrac;
    if (distSq <= fadeStartDistSq) return 1.0f;
    return std::clamp((effectiveMaxDistSq - distSq) / (effectiveMaxDistSq - fadeStartDistSq), 0.0f, 1.0f);
}

// WOWEE_M2_BATCH_DIAG: the model whose batches the draw diagnostics speak of,
// lower case; empty for none.
const std::string& m2BatchDiagModel() {
    static const std::string name = [] {
        const char* v = std::getenv("WOWEE_M2_BATCH_DIAG");
        std::string t = v ? v : "";
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return t;
    }();
    return name;
}

// Whether the batch diagnostics name this model.
bool m2BatchDiagNames(const M2ModelGPU& model) {
    const std::string& wanted = m2BatchDiagModel();
    if (wanted.empty()) return false;
    std::string lowerName = model.name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowerName.find(wanted) != std::string::npos;
}

}  // namespace

const char* M2Renderer::opaqueBatchSkip(const M2ModelGPU& model, size_t batchIndex, uint16_t lod,
                                        bool onlyBatchModel, bool suppressStars) const {
    // WOWEE_M2_ONLY_BATCH=<n> draws just that batch of the named model and
    // hides the rest.
    //
    // Everything checkable has now been checked on these trees and every
    // answer came back correct: the trunk mesh is continuous from root to tip,
    // its submesh level matches, its texture loads, its mips preserve
    // coverage, and the draw diagnostic says every batch reaches the frame. A
    // section still looks missing. Seeing the trunk by itself is the
    // difference between the trunk being wrong and the canopy eating it, and
    // no amount of reading the files can tell those apart.
    static const int kOnlyBatch = [] {
        const char* v = std::getenv("WOWEE_M2_ONLY_BATCH");
        return (v && *v) ? std::atoi(v) : -1;
    }();
    const auto& batch = model.batches[batchIndex];
    if (kOnlyBatch >= 0 && onlyBatchModel && batchIndex != static_cast<size_t>(kOnlyBatch)) {
        return "WOWEE_M2_ONLY_BATCH";
    }
    if (batch.indexCount == 0) return "no indices";
    if (!batch.shaderDrawn) return "shader 0x8000";
    if (!model.isGroundDetail && batch.submeshLevel != lod) return "submeshLevel is not this LOD";
    if (batch.batchOpacity <= kM2BatchHiddenOpacity) return "opacity is zero";
    if (!skyBatchAllowed(skyMode_, batchIndex)) return "sky batch rule";
    if (suppressStars && batch.starLayer) return "baked star layer";
    return nullptr;
}

void M2Renderer::appendGlowSprites(const M2Instance& inst, const M2ModelGPU& model, uint16_t lod,
                                   float fadeAlpha, const glm::vec3& camPos,
                                   std::vector<GlowSprite>& out, float time) const {
    // The groups the opaque pass leaves out altogether.
    if (skipGroundDetail_ && model.isGroundDetail) return;
    if (!model.vertexBuffer || !model.indexBuffer) return;
    const bool onlyBatchModel = m2BatchDiagNames(model);
    for (size_t bi = 0; bi < model.batches.size(); bi++) {
        const auto& batch = model.batches[bi];
        if (opaqueBatchSkip(model, bi, lod, onlyBatchModel, suppressBakedStars_)) continue;
        if (!m2WantsGlowSprite(glowCardOf(model, batch))) continue;

        glm::vec3 worldPos;
        if (model.isGroundFire && !model.particleEmitters.empty()) {
            worldPos = glm::vec3(std::numeric_limits<float>::max());
            for (const auto& emitter : model.particleEmitters) {
                glm::mat4 boneXform(1.0f);
                if (emitter.bone < inst.boneMatrices.size()) {
                    boneXform = inst.boneMatrices[emitter.bone];
                }
                const glm::vec3 emitterWorld = glm::vec3(
                    inst.modelMatrix * boneXform * glm::vec4(emitter.position, 1.0f));
                if (emitterWorld.z < worldPos.z) worldPos = emitterWorld;
            }
        } else {
            worldPos = animatedBatchWorldCenter(inst, batch);
        }
        // Preserved emissive glass writes opaque depth before this additive
        // point sprite. Move only the visual halo just beyond the
        // camera-facing glass surface so depth testing does not reject it;
        // the associated local light remains at the true batch center.
        if (batch.preserveGlowMesh) {
            const glm::vec3 towardCamera = camPos - worldPos;
            const float lenSq = glm::dot(towardCamera, towardCamera);
            if (lenSq > 0.0001f) {
                worldPos += towardCamera * glm::inversesqrt(lenSq) *
                    (batch.glowSize * inst.scale * 1.25f);
            }
        }
        GlowSprite gs;
        gs.worldPos = worldPos;
        if (batch.glowTint == 1 || model.isElvenLike)
            gs.color = glm::vec4(0.48f, 0.72f, 1.0f, 1.05f);
        else if (batch.glowTint == 2)
            gs.color = glm::vec4(1.0f, 0.28f, 0.22f, 1.10f);
        else
            gs.color = glm::vec4(1.0f, 0.82f, 0.46f, 1.15f);
        // Match the parent M2's distance fade instead of a separate hard
        // 180-unit cutoff, which made tunnel lights pop on.
        gs.color.a *= fadeAlpha;
        gs.size = batch.glowSize * inst.scale * (batch.preserveGlowMesh ? 2.0f : 1.45f);
        if (batch.preserveGlowMesh) gs.color.a *= 1.25f;

        // A fixture with real particle flames should read as flames with a
        // halo behind them. The sprite is sized from its glow card's
        // geometric radius, which on a chandelier spans the whole fixture - an
        // additive blob about a unit across, against candle flames of 0.15, so
        // the glow swallowed them entirely. Cap it just above what a small
        // glow card already produces (the 0.5 floor times 1.45), so candles,
        // lanterns and torches are untouched and only oversized cards are
        // clamped.
        if (!model.particleEmitters.empty() && model.isLanternLike) {
            constexpr float kMaxHaloRadius = 0.75f;
            gs.size = std::min(gs.size, kMaxHaloRadius * inst.scale);
        }

        // Fire burning inside a hearth. The sprite is a point billboard
        // carrying one depth value for the whole quad, so as soon as its
        // centre shows through the fireplace opening the entire square draws -
        // brick surround included, which reads as the fire glowing through the
        // masonry. Sized from the glow card's geometric radius these spheres
        // are wider than the opening, so keep them inside it.
        const bool hearthFire = model.isBrazierOrFire || model.isGroundFire || model.isForge;
        if (hearthFire) {
            constexpr float kMaxFireGlowRadius = 0.5f;
            gs.size = std::min(gs.size, kMaxFireGlowRadius * inst.scale);
        }

        // Flame guttering. The phase comes from the lamp's own world position,
        // so two lanterns on the same street never pulse together - a
        // synchronised row of lamps reads as a rendering artifact, not
        // firelight. Two detuned sines keep any single lamp from looping
        // visibly. Alpha and size move together, since a brighter flame also
        // looks slightly larger. Use the extracted frame's time, so preparing
        // the same view later cannot change its light.
        {
            const float flicker = lampFlicker(inst.position, time, 0.82f, 0.12f, 0.06f);
            gs.color.a *= flicker;
            gs.size *= 0.98f + 0.02f * flicker;
        }
        out.push_back(gs);
        GlowSprite halo = gs;
        halo.color.a *= batch.preserveGlowMesh ? 0.34f : 0.42f;
        halo.size *= batch.preserveGlowMesh ? 2.2f : 1.8f;
        // The halo is nearly twice the sprite, so capping only the sprite
        // would leave the bleed to the halo.
        if (hearthFire) {
            constexpr float kMaxFireHaloRadius = 0.85f;
            halo.size = std::min(halo.size, kMaxFireHaloRadius * inst.scale);
        }
        out.push_back(halo);
    }
}

namespace {

// Fill the sway half of the push constants for one model.
//
// Two modes, and the split is about who owns the idle motion. Ground clutter
// plays a sequence of its own, so mode 2 asks the shader for the player
// brush and no wind - two swings of one plant at two rates reads as a
// glitch. Everything else the wind picks up has its animation disabled by
// the classifier and gets mode 1, wind and brush both.
//
// The wind itself was written for trees: it normalised height against 20
// yards and displaced by an absolute number of model units, so a one-yard
// tuft travelled a fraction of a millimetre. Every model normalises against
// its own height now, with an amplitude interpolated between the two ends
// rather than switched at a threshold - a bush a foot taller than its
// neighbour should not sway ten times less. Both ends reproduce the numbers
// that were there: a 20-yard tree still throws 0.35 model units at the tip.
void fillSway(M2PushConstants& pc, const M2ModelGPU& mdl, bool sky) {
    const M2Sway sway = m2SwayFor(sky, mdl.isHangingCloth, mdl.shadowWindFoliage,
                                  mdl.isGroundDetail, mdl.boundMin.z, mdl.boundMax.z,
                                  mdl.isStandingCloth);
    pc.isFoliage = sway.mode;
    pc.swayRefHeight = sway.refHeight;
    pc.swayAmp = sway.amp;
    pc.plantHeight = sway.plantHeight;
}

}  // namespace

void M2Renderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera&,
                        const StarReplacement& drawStars, bool drawBakedStars) {
    if (!opaquePipeline_) return;
    recordView(cmd, perFrameSet, drawStars, drawBakedStars);
}


void M2Renderer::cullView(const Camera& camera, Frame& frame) {
    auto& drawInstances_ = frame.drawInstances_;
    auto& sortedVisible_ = frame.sortedVisible_;
    auto& transparentVisible_ = frame.transparentVisible_;
    auto& glowSprites_ = frame.glowSprites_;

    // Debug: log once when we start rendering
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        LOG_INFO("M2 render: ", scene_->instances.size(), " instances, ", models.size(), " models");
    }

    // Periodic diagnostic: report render pipeline stats every 10 seconds
    static int diagCounter = 0;
    if (++diagCounter == 600) { // ~10s at 60fps
        diagCounter = 0;
        uint32_t totalValid = 0, totalAnimated = 0, totalBonesReady = 0, totalMegaBoneOk = 0;
        for (const auto& inst : scene_->instances) {
            if (inst.cachedIsValid) totalValid++;
            if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
                totalAnimated++;
                if (!inst.boneMatrices.empty()) totalBonesReady++;
                if (inst.megaBoneOffset != 0) totalMegaBoneOk++;
            }
        }
        LOG_INFO("M2 diag: total=", scene_->instances.size(),
                 " valid=", totalValid,
                 " animated=", totalAnimated,
                 " bonesReady=", totalBonesReady,
                 " megaBoneOk=", totalMegaBoneOk,
                 " visible=", sortedVisible_.size(),
                 " draws=", lastDrawCallCount);
    }

    // Reuse persistent buffers (clear instead of reallocating)
    glowSprites_.clear();
    drawInstances_.clear();

    const bool gpuCullAvailable = cullPipeline_ != VK_NULL_HANDLE;
    const uint32_t verdictSerial = cullVerdictSerial_;

    const float maxRenderDistanceSq = smoothedRenderDist_ * smoothedRenderDist_;

    const glm::vec3 camPos = camera.getPosition();

    // Build sorted visible instance list
    sortedVisible_.clear();
    transparentVisible_.clear();
    const size_t expectedVisible = std::min(scene_->instances.size() / 3, size_t(600));
    if (sortedVisible_.capacity() < expectedVisible) {
        sortedVisible_.reserve(expectedVisible);
    }
    if (transparentVisible_.capacity() < expectedVisible / 4)
        transparentVisible_.reserve(expectedVisible / 4);

    // GPU frustum culling - build frustum for CPU fallback path and overflow instances
    Frustum frustum;
    {
        const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
        frustum.extractFromMatrix(vp);
    }
    // Same conservative ceiling as extractCull, before each instance's own
    // size/detail range is tested. Scene density must not shrink this bound.
    const float maxPossibleDistSq = cappedViewDistance() * cappedViewDistance();

    // The existing batch probe also explains models rejected before drawing.
    // Emit only when the reason changes; this is opt-in, never per-frame noise.
    if (!m2BatchDiagModel().empty()) {
        static std::unordered_map<uint32_t, uint32_t> reasons;
        for (const auto& inst : scene_->instances) {
            if (!inst.cachedModel || !m2BatchDiagNames(*inst.cachedModel)) continue;
            const float distanceSq=glm::dot(inst.position-camPos,inst.position-camPos);
            const float centreSq=glm::dot(inst.cachedCullCenter-camPos,inst.cachedCullCenter-camPos);
            const float effective=m2InstanceMaxDistSq(maxRenderDistanceSq,inst.cachedEffectiveMaxDistSqFactor,
                inst.isGameObject,M2_GAME_OBJECT_MIN_RENDER_DISTANCE,cappedViewDistance(),
                inst.cachedIsGroundDetail,groundDetailMaxDistance_);
            const bool inFrustum=frustum.intersectsSphere(inst.cachedCullCenter,inst.cachedPaddedRadius);
            const uint32_t reason=(distanceSq>maxPossibleDistSq ? 1u:0u) |
                (centreSq>maxPossibleDistSq ? 2u:0u) | (distanceSq>effective ? 4u:0u) |
                (!inFrustum ? 8u:0u) | (!inst.lastCullVisible ? 16u:0u) | (inst.hidden ? 32u:0u) |
                (!inst.cachedIsValid ? 64u:0u) | (inst.cachedIsInvisibleTrap ? 128u:0u) |
                (inst.cachedModel->hasAnimation && !inst.cachedModel->disableAnimation && inst.boneMatrices.empty() ? 256u:0u);
            auto [at,inserted]=reasons.emplace(inst.id,reason);
            if (!inserted && at->second==reason) continue;
            at->second=reason;
            LOG_WARNING("M2 VISIBILITY '",inst.cachedModel->name,"' flags=",reason,
                " eye=",camPos.x,",",camPos.y,",",camPos.z,
                " pos=",inst.position.x,",",inst.position.y,",",inst.position.z,
                " distance=",std::sqrt(distanceSq)," centreDistance=",std::sqrt(centreSq),
                " broadRange=",std::sqrt(maxPossibleDistSq)," ownRange=",std::sqrt(effective),
                " radius=",inst.cachedPaddedRadius," frustum=",inFrustum,
                " animated=",inst.cachedModel->hasAnimation," disableAnim=",inst.cachedModel->disableAnimation,
                " bones=",inst.boneMatrices.size()," boneOffset=",inst.megaBoneOffset);
        }
    }

    // The instances in columns this camera may see; every one the test below
    // would keep is among them. This walked the whole resident world - ninety
    // thousand instances in Dalaran - for the few thousand it drew.
    std::vector<uint32_t>& candidates = viewCandidates_;
    if (forceNoCull_) {
        candidates.resize(scene_->instances.size());
        for (uint32_t i = 0; i < candidates.size(); ++i) candidates[i] = i;
    } else {
        gatherViewCandidates(camPos, frustum, maxPossibleDistSq, candidates);
    }
    const uint32_t totalInstances = static_cast<uint32_t>(candidates.size());
    const bool haveLights = frameLights_ && !frameLights_->empty();

    // Visibility classification is independent per instance and was the
    // remaining monolithic M2 CPU pass. Split dense scenes across a few pool
    // workers; the caller handles the final chunk so nested use from the M2
    // render worker cannot deadlock the shared pool.
    const uint32_t chunkCount = totalInstances >= 2048
        ? std::min<uint32_t>(4, (totalInstances + 1023) / 1024)
        : 1;
    auto& chunks = visibilityChunks_;
    for (auto& chunk : chunks) chunk.clear();
    const auto reserveGrowing = [](auto& values, size_t count) {
        if (count > values.capacity())
            values.reserve(std::max(count, values.capacity() + values.capacity() / 2));
    };
    auto classifyInto = [&](std::span<VisibleChunk> into, const std::vector<uint32_t>& list,
                            uint32_t chunk, uint32_t begin, uint32_t end) {
        auto& out = into[chunk];
        reserveGrowing(out.opaque, (end - begin) / 3);
        reserveGrowing(out.transparent, (end - begin) / 12);
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t i = list[k];
            const auto& instance = scene_->instances[i];
            // A finished effect shows only the particles it has left. A
            // hidden one is in no column; this keeps it out of the walks
            // over every instance (forceNoCull_, the cull check) as well.
            if (instance.finishing || instance.hidden) continue;
            float distSq;
            float effectiveMaxDistSq;

            // Server game objects keep a distance floor instead of following the
            // ambient doodad distance down; it also feeds the fade curve below,
            // so a mailbox doesn't fade out at the doodad boundary either.
            const float instanceMaxDistSq = rendering::m2InstanceMaxDistSq(
                maxRenderDistanceSq, instance.cachedEffectiveMaxDistSqFactor,
                instance.isGameObject, rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE,
                cappedViewDistance(),
                instance.cachedIsGroundDetail, groundDetailMaxDistance_);

            if (forceNoCull_) {
                if (!instance.cachedIsValid) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                effectiveMaxDistSq = instanceMaxDistSq;
            } else if (gpuCullAvailable && instance.cullVerdictSerial == verdictSerial) {
                // Per-instance verdict scattered above - indexing visibility[]
                // directly here would read the slot of whichever instance held
                // this array position when the dispatch was recorded.
                if (!instance.lastCullVisible) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                effectiveMaxDistSq = instanceMaxDistSq;
            } else {
                if (!instance.cachedIsValid || instance.cachedIsInvisibleTrap) continue;
                glm::vec3 toCam = instance.position - camPos;
                distSq = glm::dot(toCam, toCam);
                if (distSq > maxPossibleDistSq) continue;
                effectiveMaxDistSq = instanceMaxDistSq;
                if (distSq > effectiveMaxDistSq) continue;
                float paddedRadius = instance.cachedPaddedRadius;
                if (paddedRadius > 0.0f && !frustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;
            }

            if (instance.cachedIsSkyBird && instance.cachedHasAnimation && !instance.cachedDisableAnimation) {
                constexpr float kBirdMaxDistSq =
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
                effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
                if (distSq > effectiveMaxDistSq) continue;
            }

            // A faded instance is drawn blended, all of it - see M2Instance::fade.
            const bool faded = instance.fade < 0.999f;
            const bool transparent = faded || (instance.cachedModel &&
                                               instance.cachedModel->hasTransparentBatches);
            // What the passes will draw of it, so they need not come back here.
            const uint32_t draw = static_cast<uint32_t>(out.draws.size());
            out.draws.push_back(DrawInstance{
                .modelMatrix = instance.modelMatrix,
                .model = models.at(instance.modelId),
                .interiorLight = instance.interiorLight,
                .localLights = haveLights
                    ? pickLocalLights(*frameLights_, glm::vec3(instance.modelMatrix[3]), 0.0f)
                    : kNoLocalLights,
                .position = instance.position,
                .scale = instance.scale,
                .fade = instance.fade,
                .highlight = instance.highlight,
                .animTime = instance.animTime,
                .globalSequenceTime = instance.globalSequenceTime,
                .sequenceIndex = instance.currentSequenceIndex,
                .megaBoneOffset = instance.megaBoneOffset,
                .boneCount = static_cast<uint32_t>(instance.boneMatrices.size()),
                .afloat = instance.afloat > 0,
            });
            VisibleEntry visible{.draw = draw, .modelId = instance.modelId, .distSq = distSq, .effectiveMaxDistSq = effectiveMaxDistSq};
            if (!faded) {
                out.opaque.push_back(visible);
                if (instance.cachedModel && instance.cachedModel->hasGlowSprites) {
                    const M2ModelGPU& model = *instance.cachedModel;
                    const bool needsAnimation = model.hasAnimation && !model.disableAnimation;
                    const bool posed = !needsAnimation ||
                        (!instance.boneMatrices.empty() && instance.megaBoneOffset != 0);
                    if (posed) {
                        appendGlowSprites(instance, model, m2LodFor(model, distSq),
                                          m2DistanceFade(model, distSq, effectiveMaxDistSq), camPos,
                                          out.glows, frame.time);
                    }
                }
            }
            if (transparent) out.transparent.push_back(visible);
        }
    };

    auto classifyRange = [&](uint32_t chunk, uint32_t begin, uint32_t end) {
        classifyInto(chunks, candidates, chunk, begin, end);
    };

    std::array<std::future<void>, 3> visibilityFutures;
    const uint32_t chunkSize = (totalInstances + chunkCount - 1) / chunkCount;
    for (uint32_t chunk = 0; chunk + 1 < chunkCount; ++chunk) {
        const uint32_t begin = chunk * chunkSize;
        const uint32_t end = std::min(totalInstances, begin + chunkSize);
        visibilityFutures[chunk] = core::ThreadPool::frameWorkers().submit(
            "M2 visibility: chunk",
            [&, chunk, begin, end]() { classifyRange(chunk, begin, end); });
    }
    const uint32_t lastChunk = chunkCount - 1;
    classifyRange(lastChunk, lastChunk * chunkSize, totalInstances);
    for (uint32_t chunk = 0; chunk + 1 < chunkCount; ++chunk) visibilityFutures[chunk].get();

    size_t drawCount = 0, opaqueCount = 0, transparentCount = 0, glowCount = 0;
    for (const auto& chunk : chunks) {
        drawCount += chunk.draws.size(); opaqueCount += chunk.opaque.size();
        transparentCount += chunk.transparent.size(); glowCount += chunk.glows.size();
    }
    reserveGrowing(drawInstances_, drawCount);
    reserveGrowing(sortedVisible_, opaqueCount);
    reserveGrowing(transparentVisible_, transparentCount);
    reserveGrowing(glowSprites_, glowCount);

    for (auto& chunk : chunks) {
        // Each chunk numbered its draws from zero; they follow the ones
        // before it here.
        const uint32_t base = static_cast<uint32_t>(drawInstances_.size());
        drawInstances_.insert(drawInstances_.end(), std::make_move_iterator(chunk.draws.begin()),
                              std::make_move_iterator(chunk.draws.end()));
        for (VisibleEntry& e : chunk.opaque) e.draw += base;
        for (VisibleEntry& e : chunk.transparent) e.draw += base;
        sortedVisible_.insert(sortedVisible_.end(), chunk.opaque.begin(), chunk.opaque.end());
        transparentVisible_.insert(transparentVisible_.end(), chunk.transparent.begin(),
                                   chunk.transparent.end());
        glowSprites_.insert(glowSprites_.end(), chunk.glows.begin(), chunk.glows.end());
        chunk.clear();
    }

    // WOWEE_M2_CULL_CHECK=1: classify the whole world as well, the way this
    // did before the columns, and say when the two disagree. Only a GPU verdict
    // from before the camera moved can make them differ.
    static const bool cullCheck = core::envFlagEnabled("WOWEE_M2_CULL_CHECK", false);
    if (cullCheck && !forceNoCull_ && !skyMode_) {
        std::vector<uint32_t> all(scene_->instances.size());
        for (uint32_t i = 0; i < all.size(); ++i) all[i] = i;
        std::vector<VisibleChunk> whole(1);
        classifyInto(whole, all, 0, 0, static_cast<uint32_t>(all.size()));
        static std::chrono::steady_clock::time_point lastReport{};
        const auto now = std::chrono::steady_clock::now();
        const bool differs = whole[0].opaque.size() != sortedVisible_.size() ||
                             whole[0].transparent.size() != transparentVisible_.size();
        if (now - lastReport > std::chrono::seconds(differs ? 1 : 10)) {
            lastReport = now;
            LOG_WARNING("M2 cull check: columns kept ", candidates.size(), " of ", scene_->instances.size(),
                        " to classify; visible opaque ", sortedVisible_.size(), " vs ",
                        whole[0].opaque.size(), " whole-world, transparent ",
                        transparentVisible_.size(), " vs ", whole[0].transparent.size(),
                        differs ? "  <-- DIFFERS" : "");
        }
    }

    if (!m2BatchDiagModel().empty()) {
        static std::unordered_map<uint32_t,bool> selected;
        for(const auto& inst:scene_->instances) {
            if(!inst.cachedModel || !m2BatchDiagNames(*inst.cachedModel)) continue;
            bool present=false;
            for(const auto& d:drawInstances_) if(d.model.get()==inst.cachedModel) {present=true;break;}
            auto [at,inserted]=selected.emplace(inst.id,present);
            if(inserted || at->second!=present) {
                at->second=present;
                LOG_WARNING("M2 SELECTION '",inst.cachedModel->name,"' selected=",present,
                    " candidates=",candidates.size()," bones=",inst.boneMatrices.size()," boneOffset=",inst.megaBoneOffset);
            }
        }
    }

    // The furthest doodad actually drawn, for the diagnostic that sets it
    // against the furthest terrain chunk. See Renderer::logViewDistanceDiag.
    furthestDrawnSq_ = 0.0f;
    for (const auto& e : sortedVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;
    for (const auto& e : transparentVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;

    // Whether the sky model survived culling this frame, when this is the
    // renderer that draws one.
    //
    // Reported as flickering while the camera turns and steady while it does
    // not, with everything upstream measured and holding still: the lighting
    // inputs, the model's clock, the frame time. What that leaves is the dome
    // being drawn on some frames and not others, and this says so in one line
    // per change rather than one per frame.
    // WOWEE_SKY_M2_OPAQUE_ONLY=1 drops the sky model's blended layers.
    //
    // The model is the culprit - the flicker goes with WOWEE_NO_SKY_M2 - and it
    // is not a small one: hellfireskybox.m2 carries 23 textures over 34 render
    // flags, with five transparency tracks and seven UV animations driven by
    // six global sequences. The brightening and dimming is those alpha tracks.
    // This says whether the flicker is in the blended layers or in the base the
    // opaque pass draws, which halves what is left to read.
    static const bool skyOpaqueOnly = std::getenv("WOWEE_SKY_M2_OPAQUE_ONLY") != nullptr;
    if (skyMode_ && skyOpaqueOnly) transparentVisible_.clear();

    if (skyMode_) {
        const bool drawn = !sortedVisible_.empty() || !transparentVisible_.empty();
        if (drawn != skyDiagWasDrawn_) {
            skyDiagWasDrawn_ = drawn;
            LOG_INFO("skyM2 cull: ", drawn ? "DRAWN" : "CULLED",
                     " opaque=", sortedVisible_.size(),
                     " transparent=", transparentVisible_.size(),
                     " instances=", scene_->instances.size());
        }
    }

    // Two-pass rendering: opaque/alpha-test first (depth write ON), then transparent/additive
    // (depth write OFF, sorted back-to-front) so transparent geometry composites correctly
    // against all opaque geometry rather than only against what was rendered before it.

    // Pass 1: sort by modelId for minimum buffer rebinds (opaque batches)
    //
    // Ordering the groups front-to-back was tried, on the theory that cutout
    // foliage cannot be rejected before it is shaded so the near trees should
    // lay depth down first. It moved nothing: doodads measured 14.66ms against
    // 16.55 and 13.38 either side of it, which is the middle of the spread.
    // What it did cost was a hash map over every visible instance, two index
    // vectors and a copy of the whole list, every frame, in the pass that
    // turned out to be the largest single piece of CPU work in the frame.
    std::sort(sortedVisible_.begin(), sortedVisible_.end(),
              [](const VisibleEntry& a, const VisibleEntry& b) { return a.modelId < b.modelId; });
    // Pass 2 is drawn back to front, one instance at a time.
    std::sort(transparentVisible_.begin(), transparentVisible_.end(),
              [](const VisibleEntry& a, const VisibleEntry& b) { return a.distSq > b.distSq; });
}
void M2Renderer::collectCullOutput(uint32_t frame) {
    const auto* visibility = static_cast<const uint32_t*>(cullOutputMapped_[frame]);
    if (!visibility) return;
    std::lock_guard<std::mutex> lock(cullResultMutex_);
    for (size_t k = 0; k < submittedCullIds_[frame].size(); ++k)
        collectedCull_.push_back({submittedCullIds_[frame][k], visibility[k] != 0, submittedCullScenes_[frame], submittedCullFrames_[frame]});
}

std::vector<CullAnswer> M2Renderer::takeCullAnswers() {
    std::lock_guard lock(cullResultMutex_);
    std::vector<CullAnswer> result;
    result.swap(collectedCull_);
    return result;
}

void M2Renderer::acceptCullAnswers(const std::vector<CullAnswer>& answers) {
    ++cullVerdictSerial_;
    for (const auto& result : answers) {
        if (result.sceneIdentity != scene_->identity) continue;
        const auto found = scene_->instanceIndexById.find(result.id);
        if (found == scene_->instanceIndexById.end()) continue;
        auto& inst = scene_->instances[found->second];
        inst.cullVerdictSerial = cullVerdictSerial_;
        inst.lastCullVisible = result.visible ? 1 : 0;
        inst.hizPrevCulledFrames = result.visible ? 0 : std::min(inst.hizPrevCulledFrames + 1u, 3u);
    }
}

void M2Renderer::Frame::clear() noexcept {
    drawInstances_.clear(); sortedVisible_.clear(); transparentVisible_.clear();
    glowSprites_.clear(); ribbonDraws_.clear(); particleRuns_.clear();
    ribbonVertices.clear(); particleVertices.clear(); bones.clear();
    cullInput.clear(); cullIds.clear(); depth.clear(); solarDepth.clear(); strikeDepth.clear();
    instanceEntries = 0;
    solarMatrix = overheadMatrix = strikeMatrix = glm::mat4(0.0f);
}

std::shared_ptr<const M2Renderer::Frame> M2Renderer::extract(const Camera& camera, const std::vector<LocalLight>& lights, float time, const glm::mat4& shadowMatrix, float shadowRadius,
        const glm::mat4* overheadMatrix, float overheadRadius,
        const glm::mat4* strikeMatrix, float strikeRadius) {
    auto frame = framePool_.acquire();
    frame->time = time;
    frame->cameraPosition = camera.getPosition();
    frame->skyHour = skyTimeOfDayHours_;
    frame->suppressStars = suppressBakedStars_;
    frame->skipDetail = skipGroundDetail_;
    frame->sceneIdentity = scene_->identity;
    frame->solarMatrix = shadowMatrix;
    frameLights_ = &lights;
    extractBones(*frame);
    extractCull(camera, *frame);
    cullView(camera, *frame);
    // One base record and at most one override per batch (opaque UVs or
    // transparent draws). This bounds every pass before any command is recorded.
    for(const auto& draw:frame->drawInstances_)
        if(draw.model) frame->instanceEntries+=uint64_t(1)+draw.model->batches.size();
    gatherM2Particles(*frame);
    gatherM2Ribbons(*frame);
    if (shadowMatrix != glm::mat4(0.0f)) {
        gatherReusedShadowCandidates(shadowMatrix, shadowRadius);
        frame->solarDepth.reserve(shadowCandidates_.size());
        for (uint32_t index : shadowCandidates_) {
            const auto& inst = scene_->instances[index];
            if (inst.hidden || inst.finishing || !inst.cachedModel || !inst.cachedIsValid || inst.cachedIsInvisibleTrap) continue;
            frame->solarDepth.push_back({inst.modelId, inst.modelMatrix, models.at(inst.modelId), inst.position, inst.scale});
        }
    }
    if (overheadMatrix) {
        frame->overheadMatrix = *overheadMatrix;
        gatherShadowCandidates(*overheadMatrix, overheadRadius, shadowCandidates_);
        frame->depth.reserve(shadowCandidates_.size());
        for (uint32_t index : shadowCandidates_) {
            const auto& inst = scene_->instances[index];
            if (inst.hidden || inst.finishing || !inst.cachedModel || !inst.cachedIsValid || inst.cachedIsInvisibleTrap) continue;
            frame->depth.push_back({inst.modelId, inst.modelMatrix, models.at(inst.modelId), inst.position, inst.scale});
        }
    }
    if (strikeMatrix) {
        frame->strikeMatrix = *strikeMatrix;
        gatherShadowCandidates(*strikeMatrix, strikeRadius, shadowCandidates_);
        frame->strikeDepth.reserve(shadowCandidates_.size());
        for (uint32_t index : shadowCandidates_) {
            const auto& inst = scene_->instances[index];
            if (inst.hidden || inst.finishing || !inst.cachedModel || !inst.cachedIsValid || inst.cachedIsInvisibleTrap) continue;
            frame->strikeDepth.push_back({inst.modelId, inst.modelMatrix, models.at(inst.modelId), inst.position, inst.scale});
        }
    }
    frameLights_ = nullptr;
    return frame;
}


void M2Renderer::recordStarReplacement(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                       const M2ModelGPU& model, const StarReplacement& drawStars) {
    drawStars();
    // A different renderer just bound its pipeline, descriptors and vertices.
    // Restore all M2 bindings; callers invalidate their pipeline/material cache.
    const uint32_t frame = vkCtx_->getCurrentFrame();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
    const VkDescriptorSet bones = megaBoneSet_[frame] ? megaBoneSet_[frame] : dummyBoneSet_;
    if (bones) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     pipelineLayout_, 2, 1, &bones, 0, nullptr);
    if (instanceSet_[frame]) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout_, 3, 1, &instanceSet_[frame], 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
}

void M2Renderer::recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                            const StarReplacement& drawStars, bool drawBakedStars) {
    const auto& drawInstances_ = recording_->drawInstances_;
    const auto& sortedVisible_ = recording_->sortedVisible_;

    lastDrawCallCount = 0;
    skyDiagDrawsOpaque_ = 0;
    skyDiagDrawsTransparent_ = 0;
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if(recording_->instanceEntries>instanceCapacity_[frameIndex]) return;

    // State tracking
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet currentMaterialSet = VK_NULL_HANDLE;

    // Validate per-frame descriptor set before any Vulkan commands
    if (!perFrameSet) {
        LOG_ERROR("M2Renderer::render: perFrameSet is VK_NULL_HANDLE - skipping M2 render");
        return;
    }

    // Bind per-frame descriptor set (set 0) - shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Start with opaque pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline_);
    currentPipeline = opaquePipeline_;

    // Bind dummy bone set (set 2) so non-animated draws have a valid binding.
    // Bind mega bone SSBO instead - all instances index into one buffer via boneBase.
    if (megaBoneSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &megaBoneSet_[frameIndex], 0, nullptr);
    } else if (dummyBoneSet_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &dummyBoneSet_, 0, nullptr);
    }

    // Bind instance data SSBO (set 3) - per-instance transforms, fade, bones
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }

    // Reset instance SSBO write cursor for this frame
    instanceDataCount_ = 0;
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // =====================================================================
    // Opaque pass - instanced draws grouped by (modelId, LOD)
    // =====================================================================
    // sortedVisible_ is already sorted by modelId so consecutive entries share
    // the same vertex/index buffer.  Within each model group we sub-group by
    // targetLOD to guarantee all instances in one vkCmdDrawIndexed use the
    // same batch set.  Per-instance data (model matrix, fade, bones) is
    // written to the instance SSBO; the shader reads it via gl_InstanceIndex.
    {
        struct PendingInstance {
            /// Into drawInstances_.
            uint32_t draw;
            float fadeAlpha;
            bool useBones;
            uint16_t targetLOD;
        };
        std::vector<PendingInstance> pending;
        pending.reserve(128);

        size_t visStart = 0;
        while (visStart < sortedVisible_.size()) {
            // Find group of consecutive entries with same modelId
            uint32_t groupModelId = sortedVisible_[visStart].modelId;
            size_t groupEnd = visStart;
            while (groupEnd < sortedVisible_.size() && sortedVisible_[groupEnd].modelId == groupModelId)
                groupEnd++;

            // The model the cull took from the first entry's instance, rather
            // than a models.find(groupModelId) per group.
            const M2ModelGPU* groupModel = drawInstances_[sortedVisible_[visStart].draw].model.get();
            if (!groupModel) {
                visStart = groupEnd;
                continue;
            }
            const M2ModelGPU& model = *groupModel;
            if (recording_->skipDetail && model.isGroundDetail) {
                visStart = groupEnd;
                continue;
            }
            if (!model.vertexBuffer || !model.indexBuffer) {
                visStart = groupEnd;
                continue;
            }

            bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;

            // Collect per-instance data for this model group
            pending.clear();
            for (size_t vi = visStart; vi < groupEnd; vi++) {
                const auto& entry = sortedVisible_[vi];
                const DrawInstance& instance = drawInstances_[entry.draw];

                // Distance-based fade alpha. Ground detail used to be held at
                // 0.82 here. This is the opaque pass: nothing blended it, so it
                // was a number with no effect - and now that the cutout
                // pipeline turns alpha into coverage, keeping it would punch a
                // fifth of the pixels out of every tuft of grass in reach.
                const float fadeAlpha = m2DistanceFade(model, entry.distSq, entry.effectiveMaxDistSq);

                // Bone readiness check
                if (modelNeedsAnimation && instance.boneCount == 0) continue;
                bool needsBones = modelNeedsAnimation && instance.boneCount > 0;
                if (needsBones && instance.megaBoneOffset == 0) continue;

                pending.push_back({.draw = entry.draw, .fadeAlpha = fadeAlpha, .useBones = needsBones,
                                   .targetLOD = m2LodFor(model, entry.distSq)});
            }

            if (pending.empty()) { visStart = groupEnd; continue; }

            // Sort by targetLOD so each sub-group occupies a contiguous SSBO range
            std::sort(pending.begin(), pending.end(),
                      [](const PendingInstance& a, const PendingInstance& b) { return a.targetLOD < b.targetLOD; });

            // Bind vertex/index buffers once per model group
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &vbOffset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // Write base instance data to SSBO (no texture animation - overridden for tex-anim batches)
            uint32_t baseSSBOOffset = instanceDataCount_;
            size_t writtenInstances = 0;
            for (const auto& p : pending) {
                if (instanceDataCount_ >= instanceCapacity_[frameIndex]) break;
                const DrawInstance& inst = drawInstances_[p.draw];
                auto& e = instSSBO[instanceDataCount_];
                e.model = inst.modelMatrix;
                e.uv = M2UVTransform{};
                e.uv2 = M2UVTransform{};
                e.localLights = inst.localLights;
                e.fadeAlpha = p.fadeAlpha;
                e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                e.boneCount = static_cast<int32_t>(inst.boneCount);
                e.highlight = inst.highlight;
                e.flags = inst.afloat ? 1 : 0;
                e.interiorLight = inst.interiorLight;
                instanceDataCount_++;
                ++writtenInstances;
            }

            // Drop what did not fit. The loop above stops writing at the cap,
            // but the LOD sub-groups below are ranges over `pending` and were
            // still being drawn in full - groupSSBOOffset then runs past the
            // end of the buffer and the vertex shader reads instance data that
            // is not there. That is a real out-of-bounds read on the GPU, not a
            // missing model: it fires once per instance past the cap, hundreds
            // of times a frame, and the device is lost seconds later.
            //
            // Truncating is safe here precisely because `pending` was sorted by
            // LOD before the write: the sub-groups are contiguous and in the
            // same order, so cutting the tail cuts whole instances rather than
            // splitting a range.
            if (writtenInstances < pending.size()) {
                static bool warnedInstanceCap = false;
                if (!warnedInstanceCap) {
                    warnedInstanceCap = true;
                    LOG_WARNING("M2Renderer: instance buffer full at ", instanceCapacity_[frameIndex],
                                "; dropping ", pending.size() - writtenInstances,
                                " instances of '", model.name, "' this frame");
                }
                pending.resize(writtenInstances);
            }

            // Process LOD sub-groups within this model group
            size_t lodIdx = 0;
            while (lodIdx < pending.size()) {
                uint16_t lod = pending[lodIdx].targetLOD;
                size_t lodEnd = lodIdx + 1;
                while (lodEnd < pending.size() && pending[lodEnd].targetLOD == lod) lodEnd++;
                uint32_t groupSize = static_cast<uint32_t>(lodEnd - lodIdx);
                uint32_t groupSSBOOffset = baseSSBOOffset + static_cast<uint32_t>(lodIdx);

                // What each batch of a named model does at draw time, once.
                //
                // WOWEE_M2_BATCH_DIAG says what a batch IS, at load. It could
                // not say whether the batch was drawn, and a tree reported as
                // missing a section of trunk is exactly that question: four
                // readings of the model data in a row said every batch was
                // present and correct, which it is, so the answer has to come
                // from the frame rather than from the file.
                const bool diagThisModel = m2BatchDiagNames(model);
                for (size_t bi = 0; bi < model.batches.size(); bi++) {
                    const auto& batch = model.batches[bi];
                    const char* skipped = opaqueBatchSkip(model, bi, lod, diagThisModel, recording_->suppressStars);
                    if (drawStars && recording_->suppressStars && batch.starLayer &&
                        batch.blendMode <= 1 && !batch.blendedByOpacity &&
                        !opaqueBatchSkip(model, bi, lod, diagThisModel, false)) {
                        recordStarReplacement(cmd, perFrameSet, model, drawStars);
                        currentPipeline = VK_NULL_HANDLE;
                        currentMaterialSet = VK_NULL_HANDLE;
                    }
                    if (diagThisModel) {
                        static std::set<std::pair<const void*, size_t>> saidDraw;
                        if (saidDraw.size() < 64 && saidDraw.insert({&model, bi}).second) {
                            LOG_WARNING("M2 DRAW '", model.name, "' batch ", bi,
                                        " lod=", lod, " submeshLevel=", batch.submeshLevel,
                                        " idx=", batch.indexCount,
                                        " opacity=", batch.batchOpacity,
                                        (skipped ? "  SKIPPED: " : "  drawn"),
                                        (skipped ? skipped : ""));
                        }
                    }
                    if (skipped) continue;
                    // A glow card's sprites were made by the cull
                    // (appendGlowSprites); the card itself is left out where
                    // the sprite stands in for it.
                    const M2GlowCardBatch glowCard = glowCardOf(model, batch);
                    if (m2WantsGlowSprite(glowCard) && m2GlowSpriteReplacesMesh(glowCard)) continue;

                    // Opaque gate - transparent glow cards were handled above so their
                    // sprites are generated before the mesh moves to pass 2, and
                    // so is a layer the client blends for its alpha, to be
                    // faded or left out per instance there.
                    const bool rawTransparent = (batch.blendMode >= 2) ||
                                                batch.blendedByOpacity;
                    if (rawTransparent) continue;

                    // Handle texture animation: if this batch has a per-instance UV
                    // transform, write a separate SSBO range with the right ones.
                    bool hasBatchTexAnim = (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation)
                                           || skyMode_ || batch.batchOpacity != 1.0f;
                    uint32_t drawOffset = groupSSBOOffset;
                    if (hasBatchTexAnim && instanceDataCount_ + groupSize <= instanceCapacity_[frameIndex]) {
                        drawOffset = instanceDataCount_;
                        // Hoist per-batch lookups: the transform pointer is fixed for
                        // every instance in this group; only the sampled transform
                        // varies (per-instance animTime). The second stage animates
                        // by the combo entry after the first.
                        const pipeline::M2TextureTransform* tt = nullptr;
                        const pipeline::M2TextureTransform* tt2 = nullptr;
                        if (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation) {
                            tt = m2TextureTransformAt(model.textureTransformLookup,
                                                      model.textureTransforms, batch.textureAnimIndex);
                            if (batch.secondTexture) {
                                tt2 = m2TextureTransformAt(model.textureTransformLookup, model.textureTransforms,
                                                           static_cast<size_t>(batch.textureAnimIndex) + 1);
                            }
                        }
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            const auto& p = pending[j];
                            const DrawInstance& inst = drawInstances_[p.draw];
                            M2UVTransform uv, uv2;
                            if (tt) {
                                uv = m2SampleUVTransform(*tt, inst.sequenceIndex,
                                                         inst.animTime, inst.globalSequenceTime,
                                                         model.globalSequenceDurations);
                            }
                            if (tt2) {
                                uv2 = m2SampleUVTransform(*tt2, inst.sequenceIndex,
                                                          inst.animTime, inst.globalSequenceTime,
                                                          model.globalSequenceDurations);
                            }
                            // Rebuild the entry from CPU-side data rather than copying it
                            // out of the base entry. instSSBO lives in write-combined
                            // upload memory: writing it is cheap, but reading it back is
                            // uncached and costs microseconds per instance.
                            auto& e = instSSBO[instanceDataCount_];
                            e.model = inst.modelMatrix;
                            e.uv = uv;
                            e.uv2 = uv2;
                            e.localLights = inst.localLights;
                            e.fadeAlpha = p.fadeAlpha * batch.batchOpacity;
                            if (skyMode_) {
                                const uint32_t duration = model.sequences.empty() ? 0 : model.sequences[0].duration;
                                e.fadeAlpha *= skyBatchOpacity(model.colorAlphaTracks, batch.colorTrackIndex,
                                    model.textureWeightTracks, batch.weightTrackIndex,
                                    skyAnimationTimeMs(recording_->skyHour, duration), inst.globalSequenceTime,
                                    model.globalSequenceDurations);
                            }
                            e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                            e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                            e.boneCount = static_cast<int32_t>(inst.boneCount);
                            e.highlight = inst.highlight;
                            e.flags = inst.afloat ? 1 : 0;
                            e.interiorLight = inst.interiorLight;
                            instanceDataCount_++;
                        }
                    }

                    // Keep SSBO addresses identical across the atmosphere,
                    // reflection and main sky passes; filter only the draw.
                    if (!drawBakedStars && batch.starLayer) continue;

                    // Pipeline selection (per-model/batch, not per-instance).
                    // What the batch is drawn as was decided when the model
                    // loaded - see m2BatchDrawState.
                    const M2BatchDrawState& drawState = batch.drawState;
                    VkPipeline desiredPipeline = drawState.forceCutout
                        ? cutoutPipeline_
                        : blendPipeline(m2BlendPipeline(drawState.drawBlendMode));
                    if (skyMode_ && drawState.drawBlendMode <= 1) desiredPipeline = scenePipes_.alpha;
                    if (desiredPipeline != currentPipeline) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                        currentPipeline = desiredPipeline;
                    }

                    // Bind material descriptor set (set 1)
                    if (!batch.materialSet) continue;
                    if (batch.materialSet != currentMaterialSet) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                        currentMaterialSet = batch.materialSet;
                    }

                    // Push constants + instanced draw
                    M2PushConstants pc;
                    pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
                    pc.texCoordSet2 = static_cast<int32_t>(batch.secondTexCoords);
                    fillSway(pc, model, skyMode_);
                    pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
                    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, batch.indexCount, groupSize, batch.indexStart, 0, 0);
                    if (skyMode_) ++skyDiagDrawsOpaque_;
                    lastDrawCallCount++;
                }

                lodIdx = lodEnd;
            }

            visStart = groupEnd;
        }
    }

    // With the water out of the scene pass, the blended half waits for it:
    // drawn now, the water would be laid over it.
    blendedPendingAfterWater_ = drawsBlendedAfterWater();
    if (!blendedPendingAfterWater_) recordTransparentPass(cmd, perFrameSet, drawStars, drawBakedStars);

    // How many of the sky's layers were actually drawn this frame.
    //
    // The instance-level report says DRAWN every frame, which is a different
    // question: it counts instances that survived culling, not batches that
    // reached a draw call. Every per-batch gate between the two is meant to be
    // constant here - the material flags are static, the LOD is chosen by a
    // distance that does not change for a dome centred on the camera, and the
    // batch opacity is baked at load - so this number should never move. If it
    // moves while the camera turns, the flicker is layers appearing and
    // disappearing and one of those gates is not as constant as it reads.
    if (skyMode_ && (skyDiagDrawsOpaque_ != skyDiagLastOpaque_ ||
                     skyDiagDrawsTransparent_ != skyDiagLastTransparent_)) {
        LOG_INFO("skyM2 draws: opaque=", skyDiagDrawsOpaque_,
                 " transparent=", skyDiagDrawsTransparent_,
                 " (was ", skyDiagLastOpaque_, "/", skyDiagLastTransparent_, ")");
        skyDiagLastOpaque_ = skyDiagDrawsOpaque_;
        skyDiagLastTransparent_ = skyDiagDrawsTransparent_;
    }
}

void M2Renderer::recordTransparentPass(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                       const StarReplacement& drawStars, bool drawBakedStars) {
    const auto& drawInstances_ = recording_->drawInstances_;
    const auto& transparentVisible_ = recording_->transparentVisible_;
    const auto& glowSprites_ = recording_->glowSprites_;

    if (!perFrameSet || instanceMapped_[vkCtx_->getCurrentFrame()] == nullptr) return;
    // =====================================================================
    // Pass 2: Transparent/additive batches - back-to-front per instance
    // =====================================================================
    // Transparent geometry must be drawn individually per instance in back-to-
    // front order for correct alpha compositing.  Each draw writes one
    // M2InstanceGPU entry and issues a single-instance indexed draw. The cull
    // sorted them back to front.
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if(recording_->instanceEntries>instanceCapacity_[frameIndex]) return;
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // The sets the opaque pass bound, again: this may be a different command
    // buffer from the one it recorded into.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
    if (megaBoneSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &megaBoneSet_[frameIndex], 0, nullptr);
    } else if (dummyBoneSet_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &dummyBoneSet_, 0, nullptr);
    }
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }

    uint32_t currentModelId = UINT32_MAX;
    const M2ModelGPU* currentModel = nullptr;
    bool currentModelValid = false;
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet currentMaterialSet = VK_NULL_HANDLE;

    for (const auto& entry : transparentVisible_) {
        const DrawInstance& instance = drawInstances_[entry.draw];

        // Model boundary: the model the cull took off the instance, rather
        // than a models.find() per boundary.
        if (entry.modelId != currentModelId) {
            currentModelId = entry.modelId;
            currentModelValid = false;
            currentModel = instance.model.get();
            if (!currentModel) continue;
            if (!currentModel->hasTransparentBatches) continue;
            if (!currentModel->vertexBuffer || !currentModel->indexBuffer) continue;
            currentModelValid = true;
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &currentModel->vertexBuffer, &vbOff);
            vkCmdBindIndexBuffer(cmd, currentModel->indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        }
        if (!currentModelValid) continue;

        const M2ModelGPU& model = *currentModel;

        float instanceFadeAlpha =
            m2DistanceFade(model, entry.distSq, entry.effectiveMaxDistSq) * instance.fade;
        const bool instanceFaded = instance.fade < 0.999f;
        if (model.isGroundDetail) instanceFadeAlpha *= 0.82f;

        bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
        if (modelNeedsAnimation && instance.boneCount == 0) continue;
        bool needsBones = modelNeedsAnimation && instance.boneCount > 0;
        if (needsBones && instance.megaBoneOffset == 0) continue;

        const uint16_t targetLOD = m2LodFor(model, entry.distSq);

        for (const size_t batchIndex : model.materialDrawOrder) {
            const auto& batch = model.batches[batchIndex];
            if (batch.indexCount == 0 || !batch.shaderDrawn) continue;
            if (!model.isGroundDetail && batch.submeshLevel != targetLOD) continue;
            if (!skyBatchAllowed(skyMode_, static_cast<std::size_t>(&batch - model.batches.data()))) continue;

            // Pass 2 gate: only transparent/additive batches, those the
            // client blends for their alpha - or, for a faded instance, every
            // batch, since the opaque pass left it out.
            {
                const bool rawTransparent = (batch.blendMode >= 2) ||
                                            batch.blendedByOpacity || instanceFaded;
                if (!rawTransparent) continue;
            }

            // The layer's own alpha for this instance: what the model fixes
            // for it, times where it is in its fade when that animates.
            // Nothing is drawn of a layer at zero - a spell's emitter mesh,
            // there for a moment and then gone while its particles fly.
            float layerOpacity = batch.batchOpacity;
            if (batch.animatedOpacity) {
                layerOpacity *= m2SampleBatchOpacity(
                    model.colorAlphaTracks, batch.colorTrackIndex,
                    model.textureWeightTracks, batch.weightTrackIndex,
                    instance.sequenceIndex, instance.animTime,
                    instance.globalSequenceTime, model.globalSequenceDurations);
            }
            if (layerOpacity <= kM2BatchHiddenOpacity) continue;

            if (recording_->suppressStars && batch.starLayer) {
                if (drawStars) {
                    recordStarReplacement(cmd, perFrameSet, model, drawStars);
                    currentPipeline = VK_NULL_HANDLE;
                    currentMaterialSet = VK_NULL_HANDLE;
                }
                continue;
            }

            // WOWEE_SKY_M2_SKIP_BLEND=<n> drops the sky's layers of one blend
            // mode. Its thirty-four batches are six opaque, ten alpha-blended
            // (2) and eighteen additive (4), and the two behave differently in
            // a way that matters: additive is order-independent and alpha
            // blending is not. Dropping ten or dropping eighteen says which
            // half the flicker lives in, the same way NO_SKY_M2 and
            // OPAQUE_ONLY narrowed it to the blended layers at all.
            if (skyMode_) {
                static const int skipBlend = [] {
                    const char* set = std::getenv("WOWEE_SKY_M2_SKIP_BLEND");
                    return set ? std::atoi(set) : -1;
                }();
                if (skipBlend >= 0 && batch.blendMode == skipBlend) continue;
            }

            // Skip glow sprites (made by the cull, drawn below)
            const M2GlowCardBatch glowCard = glowCardOf(model, batch);
            if (m2WantsGlowSprite(glowCard) && m2GlowSpriteReplacesMesh(glowCard)) {
                continue;
            }

            // Compute the UV transforms for this instance + batch
            //
            // WOWEE_SKY_M2_NO_TEXANIM=1 freezes the sky's scrolling. Nine of
            // its eighteen additive layers carry a texture animation, and
            // those offsets are the one thing about those layers that changes
            // between frames at all - so if the flicker survives freezing them
            // it is not in what the layers sample, it is in the geometry they
            // are drawn on.
            static const bool skyNoTexAnim =
                std::getenv("WOWEE_SKY_M2_NO_TEXANIM") != nullptr;
            const bool texAnimRuns = batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation &&
                                     !(skyMode_ && skyNoTexAnim);
            M2UVTransform uv;
            if (texAnimRuns) {
                if (const auto* tt = m2TextureTransformAt(model.textureTransformLookup,
                                                          model.textureTransforms,
                                                          batch.textureAnimIndex)) {
                    uv = m2SampleUVTransform(*tt, instance.sequenceIndex,
                                             instance.animTime, instance.globalSequenceTime,
                                             model.globalSequenceDurations);
                }
            }
            // The second stage animates by the combo entry after its first.
            M2UVTransform uv2;
            if (batch.secondTexture && texAnimRuns) {
                if (const auto* tt = m2TextureTransformAt(model.textureTransformLookup,
                                                          model.textureTransforms,
                                                          static_cast<size_t>(batch.textureAnimIndex) + 1)) {
                    uv2 = m2SampleUVTransform(*tt, instance.sequenceIndex,
                                              instance.animTime, instance.globalSequenceTime,
                                              model.globalSequenceDurations);
                }
            }

            // Write single instance entry to SSBO
            if (instanceDataCount_ >= instanceCapacity_[frameIndex]) continue;
            uint32_t drawOffset = instanceDataCount_;
            auto& e = instSSBO[instanceDataCount_];
            e.model = instance.modelMatrix;
            e.uv = uv;
            e.uv2 = uv2;
            e.localLights = instance.localLights;
            e.fadeAlpha = instanceFadeAlpha * layerOpacity;
            if (skyMode_) {
                const uint32_t duration = model.sequences.empty() ? 0 : model.sequences[0].duration;
                e.fadeAlpha *= skyBatchOpacity(model.colorAlphaTracks, batch.colorTrackIndex,
                    model.textureWeightTracks, batch.weightTrackIndex,
                    skyAnimationTimeMs(recording_->skyHour, duration), instance.globalSequenceTime,
                    model.globalSequenceDurations);
            }
            e.useBones = (needsBones && !kM2NoSkinning) ? 1 : 0;
            e.boneBase = needsBones ? static_cast<int32_t>(instance.megaBoneOffset) : 0;
            e.boneCount = static_cast<int32_t>(instance.boneCount);
            e.highlight = instance.highlight;
            e.flags = instance.afloat ? 1 : 0;
            e.interiorLight = instance.interiorLight;
            instanceDataCount_++;

            // Earlier captures still reference these SSBO addresses. Reserve
            // the same entries even when this pass omits celestial imagery.
            if (!drawBakedStars && batch.starLayer) continue;

            // Pipeline selection
            const uint8_t drawBlendMode = batch.drawState.drawBlendMode;
            VkPipeline desiredPipeline = blendPipeline(m2BlendPipeline(drawBlendMode));
            // An opaque layer of a faded instance, or one part way through
            // its own fade: blended by it.
            if ((instanceFaded || layerOpacity < kM2BatchOpaqueOpacity) && drawBlendMode <= 1) {
                desiredPipeline = blended().alpha;
            }
            if (desiredPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                currentPipeline = desiredPipeline;
            }

            if (!batch.materialSet) continue;
            if (batch.materialSet != currentMaterialSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                currentMaterialSet = batch.materialSet;
            }

            // Push constants + single-instance draw
            M2PushConstants pc;
            pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
            pc.texCoordSet2 = static_cast<int32_t>(batch.secondTexCoords);
            fillSway(pc, model, skyMode_);
            pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            if (skyMode_) ++skyDiagDrawsTransparent_;
            lastDrawCallCount++;
        }
    }

    // Render glow sprites as billboarded additive point lights
    const VkPipeline glowPipeline =
        blended().particle[static_cast<size_t>(M2BlendPipeline::AlphaAdd)];
    const ::VkBuffer glowBuffer = frameBuffer(glowVB_);
    if (!glowSprites_.empty() && glowPipeline && glowBuffer && glowTexDescSet_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glowPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 1, 1, &glowTexDescSet_, 0, nullptr);

        const M2ParticlePush particlePush{
            .alphaRef = m2AlphaRef(M2BlendPipeline::AlphaAdd),
            .fogMode = static_cast<int32_t>(m2FogMode(M2BlendPipeline::AlphaAdd)),
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(particlePush), &particlePush);

        // Glow sizes were tuned on screen-space point sprites, 500 pixels per
        // unit of size at a distance of one, judged in a 2560x1440 window
        // under the 45 degree camera. A quad half this many world units wide
        // per unit of size looks the same there; at other window sizes the
        // halo now keeps its size in the world instead of on the screen.
        constexpr float kGlowHalfWidthPerSize = 0.15f;

        // Write glow quads directly to mapped buffer (no temp vector)
        size_t uploadCount = std::min(glowSprites_.size(), MAX_GLOW_SPRITES);
        auto* dst = static_cast<M2ParticleQuad*>(frameMapped(glowVB_));
        for (size_t gi = 0; gi < uploadCount; gi++) {
            const auto& gs = glowSprites_[gi];
            // Alpha squared: the alphas were tuned while the particle shader
            // applied alpha twice, and the shader now applies it once.
            dst[gi] = M2ParticleQuad{
                .position = gs.worldPos,
                .color = glm::vec4(glm::vec3(gs.color), gs.color.a * gs.color.a),
                .halfSize = glm::vec2(gs.size * kGlowHalfWidthPerSize),
                .cell = 0.0f,
                .shape = static_cast<uint32_t>(M2ParticleShape::Billboard),
            };
        }

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &glowBuffer, &offset);
        vkCmdDraw(cmd, 4, static_cast<uint32_t>(uploadCount), 0, 0);
    }
}

void M2Renderer::renderAfterWater(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!drawsBlendedAfterWater()) return;
    drawingAfterWater_ = true;
    // Only what render() culled and sorted this frame; a frame it skipped
    // would otherwise replay the last one's lists.
    if (blendedPendingAfterWater_) {
        blendedPendingAfterWater_ = false;
        recordTransparentPass(cmd, perFrameSet);
    }
    // Particles and ribbons are built from what is alive now, not from those
    // lists, and not all of them belong to an instance render() saw: a
    // creature's are posed by CharacterRenderer. Gated on render() having
    // run, a creature's wing trails went with every frame that had no doodad
    // in it.
    renderM2Particles(cmd, perFrameSet);
    renderM2Ribbons(cmd, perFrameSet);
    drawingAfterWater_ = false;
}

bool M2Renderer::createShadowInstanceBuffers() {
    if (shadowInstanceSetLayout_) return true;
    VkDevice device = vkCtx_->getDevice();
    static_assert(sizeof(M2ShadowInstance) == 96, "M2ShadowInstance is m2_shadow.vert's std430 layout");
    static_assert(offsetof(M2ShadowPush, flags) == offsetof(ShadowPush, flags),
                  "shadow.frag reads every shadow pass's flags at one offset");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo layoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCi.bindingCount = 1;
    layoutCi.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &shadowInstanceSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: could not create the shadow casters' set layout; doodads cast no shadow");
        return false;
    }

    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .descriptorCount = kShadowTexPoolFrames};
    VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets = kShadowTexPoolFrames;
    poolCi.poolSizeCount = 1;
    poolCi.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolCi, nullptr, &shadowInstanceDescPool_) != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: could not create the shadow casters' descriptor pool; doodads cast no shadow");
        return false;
    }

    const VkDeviceSize size = VkDeviceSize(kMaxShadowInstances) * sizeof(M2ShadowInstance);
    for (uint32_t i = 0; i < kShadowTexPoolFrames; ++i) {
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocInfo{};
        if (gpuCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &shadowInstanceBuffer_[i],
                            &shadowInstanceAlloc_[i], &allocInfo) != VK_SUCCESS ||
            !allocInfo.pMappedData) {
            LOG_ERROR("M2Renderer: could not allocate the shadow casters' buffer (",
                      size / 1024, " KB); doodads cast no shadow");
            return false;
        }
        shadowInstanceMapped_[i] = allocInfo.pMappedData;

        VkDescriptorSetAllocateInfo setAi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAi.descriptorPool = shadowInstanceDescPool_;
        setAi.descriptorSetCount = 1;
        setAi.pSetLayouts = &shadowInstanceSetLayout_;
        if (vkAllocateDescriptorSets(device, &setAi, &shadowInstanceSet_[i]) != VK_SUCCESS) {
            LOG_ERROR("M2Renderer: could not allocate the shadow casters' set; doodads cast no shadow");
            return false;
        }
        VkDescriptorBufferInfo bufInfo{.buffer = shadowInstanceBuffer_[i], .offset = 0, .range = size};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = shadowInstanceSet_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    return true;
}

bool M2Renderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // The set the shadow pass binds, built the same way for all four renderers.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowParamsUBO),
                               whiteTexture_->getImageView(),
                              whiteTexture_->getSampler(), "M2Renderer", shadowParams_)) {
        return false;
    }

    // Per-frame pools for foliage shadow texture sets (one per frame-in-flight, reset each frame)
    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{};
        texPoolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    if (!createShadowInstanceBuffers()) return false;

    // Set 0: the texture and the params, as every shadow pass has them. Set 1:
    // the casters (M2ShadowInstance).
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(M2ShadowPush);
    shadowPipelineLayout_ = createPipelineLayout(
        device, {shadowParams_.layout, shadowInstanceSetLayout_}, {pc});
    if (!shadowPipelineLayout_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/m2_shadow.vert.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow fragment shader");
        return false;
    }

    // M2 vertex layout: 18 floats = 72 bytes stride; the shadow takes the
    // position (offset 0) and the first texture coordinate (offset 24).
    VkVertexInputBindingDescription vertBind{};
    vertBind.binding = 0;
    vertBind.stride = 18 * sizeof(float);
    vertBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0},                     // aPos       -> position
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 6 * sizeof(float)},     // aTexCoord  -> texCoord0
    };

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("M2Renderer shadow pipeline initialized");
    return true;
}

VkDescriptorSet M2Renderer::shadowTexSet(VkTexture* tex) {
    // Cache: texture imageView -> allocated descriptor set (avoids duplicates within frame)
    auto& texSetCache = shadowTexSetCache_;
    {
        VkImageView iv = tex->getImageView();
        auto cacheIt = texSetCache.find(iv);
        if (cacheIt != texSetCache.end()) return cacheIt->second;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = shadowTexPool_[vkCtx_->getCurrentFrame()];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shadowParams_.layout;
        if (vkCtx_->allocateDescriptorSets(ai, &set) != VK_SUCCESS) {
            return shadowParams_.set; // fallback to white texture
        }
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView = iv;
        imgInfo.sampler = tex->getSampler();
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = shadowParams_.ubo;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(ShadowParamsUBO);
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
        texSetCache[iv] = set;
        return set;
    }
}

void M2Renderer::cullShadowCasters(const glm::mat4& lightSpaceMatrix, float shadowRadius, bool sunMap,
                                   bool reuseCull) {
    // Cull once, into the two passes, grouped by model.
    //
    // This walked every instance in the world twice - once per pass - doing a
    // light-space transform for each, then drew them in whatever order they
    // sat in: the vertex and index buffers were rebound whenever consecutive
    // instances came from different models, which unsorted is most of them,
    // and a batch's texture was bound once per instance rather than once per
    // batch. With tens of thousands of instances resident that is the cost of
    // this pass, and none of it is drawing.
    // Cull casters against the light-space ortho footprint, not a
    // world-space sphere around the player. The shadow frustum extends
    // ~2000 units toward the sun, so a distant tree can legitimately
    // cast across the whole view while sitting far outside any player
    // sphere - sphere culling made such shadows pop on/off at the cull
    // boundary as the player moved (large-area flicker at low sun).
    const bool perspective = glm::length(glm::vec3(lightSpaceMatrix[0][3], lightSpaceMatrix[1][3], lightSpaceMatrix[2][3])) > 1e-6f;
    Frustum lightFrustum;
    if (perspective) lightFrustum.extractFromMatrix(lightSpaceMatrix);
    auto insideFootprint = [&](const glm::vec3& position, float scale, const M2ModelGPU& model) {
        if (perspective) return lightFrustum.intersectsSphere(position, model.boundRadius * scale * 1.5f);
        const glm::vec4 clip = lightSpaceMatrix * glm::vec4(position, 1.0f);
        // Orthographic projection: w == 1, NDC directly comparable.
        // Inflate by the model's bounding sphere converted to NDC
        // (shadowRadius ≈ frustum half-extent; overshoot is harmless).
        const float margin = (model.boundRadius * scale) / shadowRadius * 1.5f;
        if (std::abs(clip.x) > 1.0f + margin || std::abs(clip.y) > 1.0f + margin) return false;
        return clip.z >= -margin && clip.z <= 1.0f + margin;
    };


    (void)sunMap;
    (void)reuseCull;
    shadowDraws_.clear();
    shadowCasters_[0].clear();
    shadowCasters_[1].clear();
    const auto& candidates = lightSpaceMatrix == recording_->strikeMatrix ? recording_->strikeDepth
        : lightSpaceMatrix == recording_->overheadMatrix ? recording_->depth : recording_->solarDepth;
    for (const auto& source : candidates) {
        if (!insideFootprint(source.position, source.scale, *source.model)) continue;
        const uint32_t draw = static_cast<uint32_t>(shadowDraws_.size());
        shadowDraws_.push_back(source);
        const auto& model = *source.model;
        // A model's authored material participation is the same as before;
        // recording filters its solid and foliage batches separately.
        const uint32_t key = source.modelId;
        shadowCasters_[model.shadowWindFoliage ? 1 : 0].push_back({key, draw});
    }
    for (auto& bucket : shadowCasters_) if (!std::is_sorted(bucket.begin(), bucket.end())) std::sort(bucket.begin(), bucket.end());
}

void M2Renderer::recordShadowCasters(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                                     const WindGpu& wind, bool foliagePass, uint32_t& castersDrawn) {
    // The casters' data goes into this frame's buffer, the sun's map first and
    // the units' after it, and each model's casters sit side by side so a
    // batch of the model is one draw of all of them.
    const uint32_t frameIdx = vkCtx_->getCurrentFrame();
    auto* casterData = static_cast<M2ShadowInstance*>(shadowInstanceMapped_[frameIdx]);
    const VkDescriptorSet casterSet = shadowInstanceSet_[frameIdx];
    {
        // What each draw is - the texture, the alpha test, the bend - is
        // carried with it in push constants rather than written into a buffer
        // both passes share. A uniform buffer is read when a draw executes,
        // not when it is recorded, so writing it again for the foliage pass
        // decided what the solid pass's draws saw as well; a foliage batch
        // that lost its alpha test that way casts the whole leaf quad.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            0, 1, &shadowParams_.set, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            1, 1, &casterSet, 0, nullptr);

        const auto& casters = shadowCasters_[foliagePass ? 1 : 0];
        for (std::size_t g = 0; g < casters.size();) {
            const uint32_t modelId = casters[g].modelId;
            std::size_t groupEnd = g;
            while (groupEnd < casters.size() && casters[groupEnd].modelId == modelId) ++groupEnd;

            const M2ModelGPU* groupModel = shadowDraws_[casters[g].draw].model.get();
            if (!groupModel) { g = groupEnd; continue; }
            const M2ModelGPU& model = *groupModel;

            // Once per model, not once per instance: the bend comes from the
            // model's own bounds and its kind, and so do the buffers.
            const M2Sway sway = m2SwayFor(false, model.isHangingCloth,
                                          model.shadowWindFoliage, model.isGroundDetail,
                                          model.boundMin.z, model.boundMax.z,
                                          model.isStandingCloth);
            // Bent in the wind only where the main pass bends it: foliage,
            // mode 1. Ground clutter (mode 2) plays its own animation there
            // and takes no shader wind, and its shadow swaying anyway was a
            // shadow moving under a plant that did not.
            const bool windBent = foliagePass && sway.mode == 1;

            const uint32_t count = static_cast<uint32_t>(groupEnd - g);
            if (shadowInstanceCount_ + count > kMaxShadowInstances) {
                if (!shadowInstancesFullSaid_) {
                    shadowInstancesFullSaid_ = true;
                    LOG_WARNING("Shadow pass: more than ", kMaxShadowInstances,
                                " doodads cast into this frame's maps; ", model.name,
                                " and the casters after it throw no shadow while it lasts");
                }
                break;
            }
            const uint32_t firstCaster = shadowInstanceCount_;
            shadowInstanceCount_ += count;
            for (std::size_t k = g; k < groupEnd; ++k) {
                const ShadowDraw& instance = shadowDraws_[casters[k].draw];
                M2ShadowInstance& out = casterData[firstCaster + (k - g)];
                out.model = instance.modelMatrix;
                if (windBent) {
                    // The instance's own origin is what gives the wind its
                    // per-tree phase; the height and amplitude are the
                    // model's. The wind's direction is turned into the
                    // model's own space here, where the main pass turns it
                    // on the GPU: the sway is applied before the model matrix.
                    const glm::vec3 origin(instance.modelMatrix[3]);
                    const float fade = m2WindDistanceFade(glm::length(origin - recording_->cameraPosition));
                    out.sway = glm::vec4(origin.x, origin.y, sway.refHeight, sway.amp * fade);
                    out.wind = shadowWindFor(wind, instance.modelMatrix).wind;
                } else {
                    out.sway = glm::vec4(0.0f);
                    out.wind = glm::vec4(0.0f);
                }
            }

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            for (const auto& batch : model.batches) {
                // A layer that fades in and out is drawn blended, and blended
                // layers cast no shadow.
                if (batch.submeshLevel > 0 || batch.blendedByOpacity || !batch.shaderDrawn ||
                    !m2BatchCastsShadow(batch.blendMode, batch.batchOpacity,
                                        batch.glowCardLike, batch.starLayer)) continue;
                const bool alphaCutout = batch.blendMode == 1;
                // A leaf card with no texture is a shadow with no caster.
                //
                // The else below binds the white set, whose alpha test passes
                // everywhere, so a batch that arrived without its texture casts
                // the shadow of a solid quad - while the main pass skips that
                // same batch for want of a material set. The result is a black
                // rectangle on the ground under a tree that looks fine, coming
                // and going as the texture cache lets the sheet back in.
                if (alphaCutout && !batch.texture) {
                    if (!warnedShadowNoTexture_) {
                        warnedShadowNoTexture_ = true;
                        LOG_WARNING("Shadow pass: a foliage batch of ", model.name,
                                    " has no texture; skipping it rather than casting a "
                                    "solid quad. The texture cache is the usual reason.");
                    }
                    continue;
                }
                // Once per batch, where it used to be once per batch per
                // instance: the texture is the batch's, and every instance of
                // this model shares it.
                if (alphaCutout && batch.texture) {
                    VkDescriptorSet texSet = shadowTexSet(batch.texture);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowPipelineLayout_, 0, 1, &texSet, 0, nullptr);
                } else {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowPipelineLayout_, 0, 1, &shadowParams_.set, 0, nullptr);
                }

                const M2ShadowPush push{
                    .lightSpace = lightSpaceMatrix,
                    .flags = glm::ivec4(alphaCutout ? 1 : 0, alphaCutout ? 1 : 0, windBent ? 1 : 0,
                                        batch.drawState.alphaTest == 4 ? 224 : 0),
                    .windPhases = wind.phases};
                vkCmdPushConstants(cmd, shadowPipelineLayout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(M2ShadowPush), &push);
                vkCmdDrawIndexed(cmd, batch.indexCount, count, batch.indexStart, 0, firstCaster);
            }
            castersDrawn += count;
            g = groupEnd;
        }
    }
}

void M2Renderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, const WindGpu& wind,
                              const glm::vec3& /*shadowCenter*/, float shadowRadius, bool resetDescriptors,
                              bool reuseCull) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (recording_->depth.empty() && recording_->solarDepth.empty() && recording_->strikeDepth.empty()) return;

    // Reset this frame slot's texture descriptor pool (safe: fence was waited on in beginFrame)
    const uint32_t frameIdx = vkCtx_->getCurrentFrame();
    if (resetDescriptors && shadowTexPool_[frameIdx]) {
        vkCtx_->resetDescriptorPool(shadowTexPool_[frameIdx]);
    }
    // Reuse persistent map - pool reset already invalidated the sets.
    if (resetDescriptors) shadowTexSetCache_.clear();

    // How many casters each pass drew, so a shadow that comes and goes can say
    // whether its caster was culled or its texture was missing.
    uint32_t castersDrawn[2] = {0, 0};

    // The cull and the recording timed apart, under the map's own stage.
    const std::string_view stageGroup = resetDescriptors
        ? std::string_view("beginFrame/graph/shadow_pass/sun: m2")
        : std::string_view("beginFrame/graph/shadow_pass/units: m2");
    {
        CpuStage cullStage(vkCtx_->cpuStages(), stageGroup, "cull");
        cullShadowCasters(lightSpaceMatrix, shadowRadius, resetDescriptors, reuseCull);
    }
    if (resetDescriptors) shadowInstanceCount_ = 0;
    if (!shadowInstanceMapped_[frameIdx] || !shadowInstanceSet_[frameIdx]) return;
    {
        CpuStage recordStage(vkCtx_->cpuStages(), stageGroup, "record");
        // Pass 1: non-foliage (no wind displacement)
        recordShadowCasters(cmd, lightSpaceMatrix, wind, false, castersDrawn[0]);
        // Pass 2: foliage (wind displacement enabled, per-batch alpha-tested textures)
        recordShadowCasters(cmd, lightSpaceMatrix, wind, true, castersDrawn[1]);
    }

    // Contact coverage differs from the solar footprint; do not report that as flicker.
    if (!resetDescriptors) return;

    // A shadow that flickers is a caster that was drawn last frame and is not
    // drawn this one. The cull is in light space and the light turns with the
    // hour, so a tree on the boundary can cross it while the player stands
    // still - and from the ground that reads as the shadow blinking. Said at
    // most once a second, and only when the count actually swings, so an
    // ordinary walk through a forest stays quiet.
    {
        const uint32_t foliageNow = castersDrawn[1];
        const uint32_t foliageWas = lastFoliageCasters_;
        lastFoliageCasters_ = foliageNow;
        const uint32_t larger = std::max(foliageNow, foliageWas);
        const uint32_t delta = larger - std::min(foliageNow, foliageWas);
        if (foliageWas != 0 && larger >= 8 && delta * 10 > larger) {
            static std::chrono::steady_clock::time_point lastLog{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                LOG_WARNING("Shadow casters swung ", foliageWas, " -> ", foliageNow,
                            " foliage instances in one frame (", castersDrawn[0],
                            " solid). A shadow that blinks with the player standing "
                            "still is one of these crossing the light-space cull.");
            }
        }
    }
}

} // namespace rendering
} // namespace wowee
