#include "rendering/shadow_params.hpp"
#include "rendering/face_culling.hpp"
#include "rendering/m2_material.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_sky_material.hpp"
#include "rendering/m2_sway.hpp"
#include "rendering/m2_blend_mode.hpp"
#include "rendering/m2_glow_card.hpp"
#include "rendering/m2_particle_emission.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/m2_model_classifier.hpp"
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
#include "rendering/m2_anim_clock.hpp"
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
#include <tuple>
#include <cmath>
#include <cstdlib>
#include <random>
#include <limits>
#include <future>
#include <iterator>
#include <span>
#include <thread>

#include <set>
#include <map>
#include <bit>
#include <numeric>
#include <utility>

namespace wowee {
namespace rendering {

float M2Renderer::animTimeOf(const M2Instance& instance) const {
    const bool stepped = instance.cachedHasAnimation && !instance.cachedDisableAnimation;
    return m2AnimTimeAt(animClockMs_, instance.animStartMs, instance.animDuration, stepped,
                        !stepped && instance.cachedHasEmitters);
}

float M2Renderer::globalSequenceTimeOf(const M2Instance& instance) const {
    return static_cast<float>(animClockMs_ - instance.globalSequenceStartMs);
}

void M2Renderer::setAnimTime(M2Instance& instance, float timeMs) const {
    instance.animStartMs = animClockMs_ - timeMs;
}

/// Starts a new instance's animation at its own random point.
///
/// Both spawn paths need this: the one that takes a position and the one that
/// takes a whole matrix, and each had its own copy. Its skeleton is posed on
/// the GPU from this state whenever it is drawn (skeletonPass_).
void M2Renderer::startInstanceAnimation(const M2ModelGPU& model, M2Instance& instance) {
    if (!model.sequences.empty()) {
        instance.currentSequenceIndex = 0;
        instance.idleSequenceIndex = 0;
        instance.animDuration = static_cast<float>(model.sequences[0].duration);
        setAnimTime(instance, static_cast<float>(instance.random.below(std::max(1u, model.sequences[0].duration))));
        instance.variationTimer = instance.random.uniform(rendering::M2_VARIATION_TIMER_MIN_MS, rendering::M2_VARIATION_TIMER_MAX_MS);
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
    setAnimTime(instance, static_cast<float>(instance.random.below(model.sequences[0].duration)));
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
    instance.random = InstanceRandom::at(position);
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
    // Both clocks start now; the seeding below may move the sequence's.
    instance.animStartMs = animClockMs_;
    instance.globalSequenceStartMs = animClockMs_;

    // Initialize animation: play first sequence (usually Stand/Idle)
    const auto& mdl = mdlRef;
    if (mdl.hasAnimation && !mdl.disableAnimation) {
        startInstanceAnimation(mdlRef, instance);
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
    if (scene_->instances[idx].cachedIsBoat) scene_->pendingAfloatIds_.push_back(scene_->instances[idx].id);
    scene_->instanceIndexById[placed.id] = idx;
    {
        const uint32_t slot = scene_->slots.add(placed.id, placed.modelId);
        if (scene_->indexOfSlot.size() <= slot) scene_->indexOfSlot.resize(slot + 1);
        scene_->indexOfSlot[slot] = static_cast<uint32_t>(idx);
    }
    scene_->cullGrid_.add();
    fileInCullGrid(idx, InstanceChange::Placed);
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
    instance.random = InstanceRandom::at(position);
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
    // Both clocks start now; the seeding below may move the sequence's.
    instance.animStartMs = animClockMs_;
    instance.globalSequenceStartMs = animClockMs_;

    // Initialize animation
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        startInstanceAnimation(mdl2, instance);
    } else {
        // A model with no skeleton can still have particle emitters, and their
        // rate and lifespan tracks are sampled at animTime. Starting every
        // instance at zero puts a courtyard of identical torches in lockstep,
        // so the phase is spread.
        //
        // createInstance above now does the same.
        setAnimTime(instance, instance.random.uniform(0.0f, 10000.0f));
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
    if (scene_->instances[idx].cachedIsBoat) scene_->pendingAfloatIds_.push_back(scene_->instances[idx].id);
    scene_->instanceIndexById[placed.id] = idx;
    {
        const uint32_t slot = scene_->slots.add(placed.id, placed.modelId);
        if (scene_->indexOfSlot.size() <= slot) scene_->indexOfSlot.resize(slot + 1);
        scene_->indexOfSlot[slot] = static_cast<uint32_t>(idx);
    }
    scene_->cullGrid_.add();
    fileInCullGrid(idx, InstanceChange::Placed);
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

void M2Renderer::update(float deltaTime, const glm::mat4& view, const glm::mat4& projection) {  // NOLINT(readability-function-size)
    const glm::vec3 cameraPos = glm::vec3(glm::inverse(view)[3]);
    const glm::mat4 viewProjection = projection * view;
    ZoneScopedN("M2Renderer::update");
    // The ribbons', lights' and glow cards' bones as the GPU posed them, and
    // what it last said of the particles.
    takeReadBones();
    takeParticleReads();
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
    // One clock for every instance. An instance keeps the reading at which its
    // sequence started and its time is worked out when something reads it
    // (animTimeOf), so the instances the bone loop below does not step - most
    // of a city - cost nothing here. Advancing a total on each of them was a
    // pass over every instance every frame.
    //
    // An instance the bone loop does not step loops at the length of its own
    // sequence (see m2AnimTimeAt). A texture track is sampled against that time
    // and holds its last key once the time runs past the end, so a waterfall
    // with no bones of its own scrolled once after it loaded and then stood
    // still, and one with emitters - wrapped at a fixed 3.3 s whatever its
    // sequence - stopped for the rest of each lap. A hidden instance's clocks
    // stop (setInstancesHidden), so it resumes where it was shown.
    animClockMs_ += dtMs;

    // A boat asks once whether it is on water - the canal may stream in after
    // it does, so it keeps asking for a while - and rides it if so. A rowboat
    // pulled up on a beach is the same model and stays put. Only boats with
    // the question still open are on the list.
    if (waterHeightAt_) {
        auto& pending = scene_->pendingAfloatIds_;
        for (size_t k = 0; k < pending.size();) {
            auto idxIt = scene_->instanceIndexById.find(pending[k]);
            if (idxIt != scene_->instanceIndexById.end() && idxIt->second < scene_->instances.size()) {
                auto& instance = scene_->instances[idxIt->second];
                if (!instance.hidden) {
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
                if (instance.afloat < 0) {
                    ++k;
                    continue;
                }
            }
            // Answered, or the boat is gone.
            pending[k] = pending.back();
            pending.pop_back();
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
        const float skyAnimTime = animTimeOf(sky);
        const bool restarted = skyAnimTime < skyDiagAnimTime_;
        if (restarted || sky.id != skyDiagInstanceId_ ||
            skyAnimTime - skyDiagAnimTime_ > 1000.0f) {
            LOG_INFO("skyM2: instance=", sky.id, " instances=", scene_->instances.size(),
                     " animTime=", skyAnimTime, " duration=", sky.animDuration,
                     " gsTime=", globalSequenceTimeOf(sky), " dtMs=", dtMs,
                     restarted ? " RESTARTED" : "");
            skyDiagInstanceId_ = sky.id;
            skyDiagAnimTime_ = skyAnimTime;
        }
    }

    // Update animated instances' animation state; their skeletons are posed
    // on the GPU from it (skeletonPass_). The clock above moves every
    // instance at speed 1. Here an instance's own
    // speed moves its start: back for faster, forward for slower or held.
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

        instance.animStartMs -= static_cast<double>(dtMs) * (instance.animSpeed - 1.0f);

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
        if (instance.animSpeed < 0.0f && animTimeOf(instance) <= 0.0f) {
            setAnimTime(instance, 0.0f);
            if (instance.nextAnimation) {
                advanceToNextAnimation(instance);
            } else {
                instance.animSpeed = 0.0f;
            }
        }
        const float animTime = animTimeOf(instance);
        if (instance.animDuration > 0.0f && animTime >= instance.animDuration &&
            instance.nextAnimation) {
            advanceToNextAnimation(instance);
        } else if (instance.animDuration > 0.0f && animTime >= instance.animDuration) {
            if (instance.holdAtEnd) {
                // Stay on the last frame. A door's open sequence ends with the
                // door open, and that is the pose the server is describing.
                setAnimTime(instance, instance.animDuration);
                instance.animSpeed = 0.0f;
            } else if (instance.playingVariation) {
                instance.playingVariation = false;
                instance.currentSequenceIndex = instance.idleSequenceIndex;
                if (instance.idleSequenceIndex < static_cast<int>(model.sequences.size())) {
                    instance.animDuration = static_cast<float>(model.sequences[instance.idleSequenceIndex].duration);
                }
                setAnimTime(instance, 0.0f);
                instance.variationTimer = instance.random.uniform(rendering::M2_LOOP_VARIATION_TIMER_MIN_MS, rendering::M2_LOOP_VARIATION_TIMER_MAX_MS);
            } else {
                // Whole laps: the start moves on by them, and the time keeps
                // its place within the sequence.
                const double duration = std::max(1.0f, instance.animDuration);
                const double elapsed = animClockMs_ - instance.animStartMs;
                instance.animStartMs += std::floor(elapsed / duration) * duration;
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
                    setAnimTime(instance, 0.0f);
                } else {
                    instance.variationTimer = instance.random.uniform(rendering::M2_IDLE_VARIATION_TIMER_MIN_MS, rendering::M2_IDLE_VARIATION_TIMER_MAX_MS);
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

    // The emitters that run this frame. Their particles are emitted and moved
    // on the GPU (m2_particles.comp), over this update's time; here they are
    // only marked, and given room the first time they run.
    ++particleSerial_;
    particleDt_ = deltaTime;
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
                LOG_WARNING("M2 PARTICLES '", instance.cachedModel->name, "' instance ", instance.id,
                            " gameObject=", instance.isGameObject ? 1 : 0,
                            " dist=", std::sqrt(distSq), " max=", std::sqrt(cachedMaxRenderDistSq_),
                            " emitters=", instance.cachedModel->particleEmitters.size(),
                            " live=", instance.liveParticles, " reach=", instance.particleReach,
                            " bones=", instance.boneMatrices.size(),
                            " seq=", instance.currentSequenceIndex, " animTime=", animTimeOf(instance),
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
        if (placeParticles(instance)) instance.particlesRanAt = particleSerial_;
    }

}

// Diagnostic: WOWEE_M2_NO_SKINNING=1 renders every M2 in its bind pose by
// telling the shader to ignore bones, separating a skinning artifact from one
// drawn by the particle or ribbon systems.
static const bool kM2NoSkinning = envFlagEnabled("WOWEE_M2_NO_SKINNING");

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
    // prepareRender runs after this slot's fence, before any M2 command is
    // recorded. The other slot's descriptor and buffers remain untouched.
    if(!makeDrawStorage(slot,capacity)) return fail("mapped GPU allocation failed");
    LOG_INFO("M2 instance storage: slot ",slot," grew to ",capacity," records (",
        VkDeviceSize(capacity)*sizeof(M2InstanceGPU)/1024," KiB), frame requires ",entries);
    return true;
}

uint32_t& M2Renderer::frameDrawCursor() {
    const uint64_t frame = vkCtx_->frameNumber();
    if (frame != drawCursorFrame_) {
        drawCursorFrame_ = frame;
        drawCursor_ = 0;
    }
    return drawCursor_;
}

bool M2Renderer::makeDrawStorage(uint32_t slot, uint32_t capacity) {
    const VmaAllocator allocator = vkCtx_->getAllocator();
    const auto make = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, AllocatedBuffer& out) {
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (gpuCreateBuffer(allocator, &bci, &aci, &out.buffer, &out.allocation, &out.info) != VK_SUCCESS) {
            out = {};
            return false;
        }
        if (!out.info.pMappedData) {
            destroyBuffer(allocator, out);
            return false;
        }
        return true;
    };
    // An entry per drawn instance and batch at most, so as many draws a pass.
    const VkDeviceSize draws = VkDeviceSize(capacity) * kDrawRoomPerEntry;
    AllocatedBuffer instances, records, commands;
    if (!make(VkDeviceSize(capacity) * sizeof(M2InstanceGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, instances) ||
        !make(draws * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, records) ||
        // Written by m2_draws.comp as well as the CPU.
        !make(draws * sizeof(VkDrawIndexedIndirectCommand),
              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, commands)) {
        if (instances.buffer) destroyBuffer(allocator, instances);
        if (records.buffer) destroyBuffer(allocator, records);
        return false;
    }
    const VkDescriptorBufferInfo infos[] = {{instances.buffer, 0, VK_WHOLE_SIZE}, {records.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t w = 0; w < 2; ++w) {
        writes[w] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[w].dstSet = instanceSet_[slot];
        writes[w].dstBinding = w == 0 ? 0 : 3;
        writes[w].descriptorCount = 1;
        writes[w].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[w].pBufferInfo = &infos[w];
    }
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
    DeferredResources retire(*vkCtx_);
    if (instanceBuffer_[slot]) retire.buffer(instanceBuffer_[slot], instanceAlloc_[slot]);
    if (drawRecords_[slot].buffer) retire.buffer(drawRecords_[slot]);
    if (drawCommands_[slot].buffer) retire.buffer(drawCommands_[slot]);
    instanceBuffer_[slot] = instances.buffer;
    instanceAlloc_[slot] = instances.allocation;
    instanceMapped_[slot] = instances.info.pMappedData;
    drawRecords_[slot] = records;
    drawCommands_[slot] = commands;
    instanceCapacity_[slot] = capacity;
    return true;
}

void M2Renderer::prepareRender(uint32_t frameIndex, const Camera&) {
    prepareInstanceStorage(frameIndex, recording_->instanceEntries);
}

void M2Renderer::poseSkeletons(VkCommandBuffer cmd, uint32_t frameIndex, const PosedBones& characters) {
    if (frameIndex >= 2) return;
    // A frame the pass could not pose (said) draws none of its skinned models
    // and runs none of its particles, which stand on those bones.
    posed_[frameIndex] = skeletonPass_.record(cmd, frameIndex, recording_->skeleton);
    const VkDescriptorSet bones = skeletonPass_.drawSet(frameIndex);
    particlesRun_[frameIndex] =
        posed_[frameIndex] &&
        particlePass_.record(cmd, frameIndex, recording_->particles, bones, characters.set ? characters.set : bones,
                             characters.set && characters.posed);
}

namespace {
uint16_t m2LodFor(const M2ModelGPU& model, float distSq);
void sayDrawRoomFull(uint32_t capacity);
}  // namespace

void M2Renderer::prepareViewDraws(VkCommandBuffer cmd, uint32_t frameIndex) {
    viewBuckets_.clear();
    skyDraws_.clear();
    drawFrame_.clear();
    viewDrawsReady_ = false;
    instanceDataCount_ = 0;
    preparedRecordEnd_ = 0;
    if (frameIndex >= 2 || !recording_) return;
    const auto& drawInstances = recording_->drawInstances_;
    const auto& visible = recording_->sortedVisible_;
    if (!prepareInstanceStorage(frameIndex, recording_->instanceEntries)) return;
    // Where they stand is in the instance table; without it (no memory,
    // logged by ensureSlotTables) nothing is drawn.
    if (!drawInstances.empty() && !instanceSetReady(frameIndex)) return;
    auto* records = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // The base records, a model's instances in a row and each level of
    // detail's in a row of its own: a group.
    struct Group {
        const M2ModelGPU* model;
        uint32_t lod;
        uint32_t first;
        uint32_t count;
        uint32_t ownFirst = 0;
    };
    std::vector<Group> groups;
    struct Pending {
        uint32_t draw;
        bool useBones;
        uint16_t lod;
    };
    std::vector<Pending> pending;
    for (size_t start = 0; start < visible.size();) {
        const uint32_t modelId = visible[start].modelId;
        size_t end = start;
        while (end < visible.size() && visible[end].modelId == modelId) ++end;
        // The model the cull took from the first entry's instance.
        const M2ModelGPU* model = drawInstances[visible[start].draw].model.get();
        const size_t groupStart = start;
        start = end;
        if (!model || !model->geometry.placed() || !model->drawList.placed()) continue;
        if (recording_->skipDetail && model->isGroundDetail) continue;
        const bool needsAnimation = model->hasAnimation && !model->disableAnimation;
        pending.clear();
        for (size_t v = groupStart; v < end; ++v) {
            const DrawInstance& instance = drawInstances[visible[v].draw];
            // Skinned when the GPU posed it; an animated model with no
            // skeleton in the pool is drawn in its bind pose.
            const bool useBones = needsAnimation && instance.boneCount > 0;
            if (useBones && !posed_[frameIndex]) continue;
            pending.push_back({.draw = visible[v].draw, .useBones = useBones,
                               .lod = m2LodFor(*model, visible[v].distSq)});
        }
        std::sort(pending.begin(), pending.end(),
                         [](const Pending& a, const Pending& b) { return a.lod < b.lod; });
        for (size_t p = 0; p < pending.size();) {
            const uint16_t lod = pending[p].lod;
            const auto first = instanceDataCount_;
            for (; p < pending.size() && pending[p].lod == lod; ++p) {
                if (instanceDataCount_ >= instanceCapacity_[frameIndex]) break;
                const DrawInstance& inst = drawInstances[pending[p].draw];
                M2InstanceGPU& e = records[instanceDataCount_];
                e.slot = inst.slot;
                e.uv = M2UVTransform{};
                e.uv2 = M2UVTransform{};
                e.localLights = inst.localLights;
                e.fadeAlpha = 1.0f;
                e.useBones = (pending[p].useBones && !kM2NoSkinning) ? 1 : 0;
                e.boneBase = pending[p].useBones ? static_cast<int32_t>(inst.boneBase) : 0;
                e.boneCount = static_cast<int32_t>(inst.boneCount);
                e.highlight = inst.highlight;
                e.flags = inst.afloat ? 1 : 0;
                drawFrame_.clocks.emplace_back(inst.animTime, inst.globalSequenceTime,
                                               std::bit_cast<float>(int32_t(inst.sequenceIndex)), 0.0f);
                ++instanceDataCount_;
            }
            while (p < pending.size() && pending[p].lod == lod) ++p;
            const uint32_t count = instanceDataCount_ - first;
            if (count > 0 && !model->levelDraws[lod].entries.empty())
                groups.push_back({.model = model, .lod = lod, .first = first, .count = count});
        }
    }
    if (groups.empty()) {
        preparedRecordEnd_ = instanceDataCount_;
        return;
    }

    // The records of each group's batches that need their own, after the
    // base records.
    for (Group& g : groups) {
        const uint64_t own = uint64_t(g.model->levelDraws[g.lod].ownCount) * g.count;
        if (instanceDataCount_ + own > instanceCapacity_[frameIndex]) {
            static core::LogBudget budget(4, "M2 own records room");
            if (budget.take())
                LOG_WARNING("M2Renderer: this frame's records filled the instance room at ",
                            instanceCapacity_[frameIndex], " - its models are not drawn this frame");
            preparedRecordEnd_ = instanceDataCount_;
            return;
        }
        g.ownFirst = instanceDataCount_;
        instanceDataCount_ += static_cast<uint32_t>(own);
    }

    // Each group's buckets' places among the frame's commands. In the world
    // the commands of a bucket - a pipeline, its faces and a geometry page -
    // follow one another, the buckets in the order they are drawn; in the
    // sky, the model's order.
    uint32_t& cursor = frameDrawCursor();
    const uint32_t room = drawRoom(frameIndex);
    uint32_t total = 0;
    if (skyMode_) {
        for (const Group& g : groups) {
            const auto& level = g.model->levelDraws[g.lod];
            for (const auto& entry : level.entries) {
                drawFrame_.offsets.push_back(cursor + total);
                skyDraws_.push_back({.model = g.model, .entry = entry, .command = cursor + total});
                ++total;
            }
        }
    } else {
        // The order the buckets are drawn in: phase by phase, so each model
        // keeps its own order; within a phase the plain opaque first, then
        // the alpha tested and the cut out, then the rest.
        const auto rank = [](uint8_t variant) -> uint32_t {
            if (variant == static_cast<uint8_t>(M2BlendPipeline::Opaque)) return 0;
            if (variant == static_cast<uint8_t>(M2BlendPipeline::AlphaTest)) return 1;
            if (variant == kVariantCutout) return 2;
            return 3 + variant;
        };
        const auto bucketKey = [&](uint16_t levelKey, int32_t page) {
            const uint64_t phase = levelKey >> kDrawPhaseShift;
            const auto variant = static_cast<uint8_t>((levelKey >> 1) & 15u);
            return (phase << 48) | (uint64_t(rank(variant)) << 40) | (uint64_t(levelKey & 1u) << 32) |
                   uint32_t(page);
        };
        std::map<uint64_t, uint32_t> counts;
        for (const Group& g : groups) {
            const auto& level = g.model->levelDraws[g.lod];
            for (size_t b = 0; b < level.buckets.size(); ++b) {
                uint32_t& count = counts[bucketKey(level.buckets[b], g.model->geometry.page)];
                drawFrame_.offsets.push_back(count);
                count += level.bucketCounts[b];
            }
        }
        std::map<uint64_t, uint32_t> firsts;
        for (const auto& [key, count] : counts) {
            firsts[key] = cursor + total;
            total += count;
        }
        size_t at = 0;
        for (const Group& g : groups) {
            const auto& level = g.model->levelDraws[g.lod];
            for (size_t b = 0; b < level.buckets.size(); ++b, ++at) {
                const uint64_t key = bucketKey(level.buckets[b], g.model->geometry.page);
                drawFrame_.offsets[at] += firsts[key];
            }
        }
        // The buckets as they are drawn; a page bound through any model's
        // range of it.
        std::map<uint64_t, ViewBucket> buckets;
        for (const Group& g : groups) {
            const auto& level = g.model->levelDraws[g.lod];
            for (const uint16_t levelKey : level.buckets) {
                const uint64_t key = bucketKey(levelKey, g.model->geometry.page);
                auto [it, made] = buckets.try_emplace(key);
                if (made)
                    it->second = {.variant = static_cast<uint8_t>((levelKey >> 1) & 15u),
                                  .twoSided = (levelKey & 1u) != 0,
                                  .page = g.model->geometry, .first = firsts.at(key), .count = counts.at(key)};
            }
        }
        for (auto& [key, bucket] : buckets) viewBuckets_.push_back(bucket);
    }
    if (cursor + total > room) {
        sayDrawRoomFull(room);
        viewBuckets_.clear();
        skyDraws_.clear();
        preparedRecordEnd_ = instanceDataCount_;
        return;
    }
    cursor += total;

    // The groups as the pass reads them; the sky's opacity of the hour for
    // each of its own records.
    for (size_t i = 0, offset = 0; i < groups.size(); ++i) {
        const Group& g = groups[i];
        const auto& level = g.model->levelDraws[g.lod];
        M2DrawGroupGPU row;
        row.listPage = static_cast<uint32_t>(g.model->drawList.page);
        row.listBlob = g.model->drawList.first;
        row.lod = g.lod;
        row.firstRecord = g.first;
        row.instanceCount = g.count;
        row.ownFirst = g.ownFirst;
        row.offsetsFirst = static_cast<uint32_t>(offset);
        offset += level.buckets.size();
        if (g.model->textureAnimations.placed()) {
            row.uvPage = static_cast<uint32_t>(g.model->textureAnimations.page);
            row.uvBlob = g.model->textureAnimations.first;
        }
        if (skyMode_) {
            row.opacityFirst = static_cast<uint32_t>(drawFrame_.skyOpacity.size());
            const M2ModelGPU& model = *g.model;
            const uint32_t duration = model.sequences.empty() ? 0 : model.sequences[0].duration;
            const float skyTime = skyAnimationTimeMs(recording_->skyHour, duration);
            // In the order the pass numbers them: the level's own entries,
            // each over the group's instances.
            for (const auto& entry : level.entries) {
                const auto& batch = model.batches[entry.batch];
                for (uint32_t k = 0; k < g.count; ++k) {
                    const float globalTime = drawFrame_.clocks[g.first + k].y;
                    drawFrame_.skyOpacity.push_back(skyBatchOpacity(
                        model.colorAlphaTracks, batch.colorTrackIndex, model.textureWeightTracks,
                        batch.weightTrackIndex, skyTime, globalTime, model.globalSequenceDurations));
                }
            }
        }
        drawFrame_.groups.push_back(row);
    }
    drawFrame_.pages = skeletons_.pageBuffers();
    drawFrame_.pagesVersion = skeletons_.version();
    viewDrawsReady_ = drawPass_.record(cmd, frameIndex, drawFrame_,
                                       {.instances = instanceBuffer_[frameIndex],
                                        .commands = drawCommands_[frameIndex].buffer,
                                        .drawMaterials = drawRecords_[frameIndex].buffer});
    preparedRecordEnd_ = instanceDataCount_;
}

SkeletonRequestGPU M2Renderer::skeletonRequest(const M2Instance& instance, uint32_t outBase) const {
    // Diagnostic: WOWEE_M2_NO_BILLBOARD=1 leaves every billboard bone as
    // authored, which tells a billboard fault from anything else.
    static const bool kNoBillboard = std::getenv("WOWEE_M2_NO_BILLBOARD") != nullptr;
    const SkeletonPool::Skeleton& skeleton = instance.cachedModel->skeleton;
    SkeletonRequestGPU request;
    request.page = static_cast<uint32_t>(skeleton.page);
    request.blob = skeleton.first;
    request.outBase = outBase;
    request.flags = SkeletonRequestGPU::kTinyScaleIsOne | (kNoBillboard ? 0u : SkeletonRequestGPU::kTurnBillboards);
    request.sequence = instance.currentSequenceIndex;
    request.time = animTimeOf(instance);
    request.globalTime = globalSequenceTimeOf(instance);
    request.placement = instance.modelMatrix;
    return request;
}

uint32_t M2Renderer::poseOf(size_t index, Frame& frame) {
    const auto [at, asked] = posedThisFrame_.try_emplace(static_cast<uint32_t>(index), 0u);
    if (!asked) return at->second;
    const M2Instance& inst = scene_->instances[index];
    SkeletonFrame& skeleton = frame.skeleton;
    const uint32_t outBase = skeleton.boneCount;
    at->second = static_cast<uint32_t>(skeleton.requests.size());
    skeleton.requests.push_back(skeletonRequest(inst, outBase));
    for (const uint16_t bone : inst.cachedModel->readBones)
        skeleton.reads.push_back({.owner = inst.id, .bone = bone, .source = outBase + bone});
    skeleton.boneCount += inst.cachedModel->skeleton.boneCount;
    return at->second;
}

void M2Renderer::poseUnseenLights(const Camera& camera, Frame& frame) {
    // Out of view, a doodad within ten yards of the camera still shines
    // (gatherLocalLights), on bones the GPU poses for it alone.
    const glm::vec3 eye = camera.getPosition();
    for (const size_t index : scene_->lightInstanceIndices_) {
        if (index >= scene_->instances.size()) continue;
        const M2Instance& inst = scene_->instances[index];
        if (!inst.cachedModel || inst.cachedModel->readBones.empty() || !inst.cachedModel->skeleton.placed() ||
            !inst.cachedHasAnimation || inst.cachedDisableAnimation || !inst.cachedIsValid || inst.hidden)
            continue;
        const glm::vec3 toEye = inst.position - eye;
        if (glm::dot(toEye, toEye) <= 100.0f) poseOf(index, frame);
    }
}

void M2Renderer::takeReadBones() {
    for (const SkeletonPass::ReadBone& read : skeletonPass_.takeReadBones()) {
        const auto at = scene_->instanceIndexById.find(read.owner);
        if (at == scene_->instanceIndexById.end()) continue;  // removed since
        M2Instance& inst = scene_->instances[at->second];
        if (!inst.cachedModel) continue;
        const uint32_t boneCount = inst.cachedModel->skeleton.boneCount;
        if (read.bone >= boneCount) continue;
        if (inst.boneMatrices.size() != boneCount) inst.boneMatrices.assign(boneCount, glm::mat4(1.0f));
        inst.boneMatrices[read.bone] = read.matrix;
    }
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

    frame.cullGuard = cullGuardFor(camera);
    frame.maxPossibleDistSq = maxPossibleDistSq;
    frame.cullBaseMaxDistSq = maxRenderDistanceSq;
    frame.cullViewDistance = cappedViewDistance();
    frame.cullGroundDetailMaxDistance = groundDetailMaxDistance_;
    const uint32_t serial = ++cullSerial_;
    frame.cullSerial = serial;

    // The table mirrors one scene. Another in its place - cleared, swapped -
    // and the table is cleared and filled from it whole.
    InstanceSlots& slots = scene_->slots;
    if (cullTableScene_ != scene_->identity) {
        cullTableScene_ = scene_->identity;
        cullResetSerial_ = serial;
        cullPendingSlots_.clear();
        std::fill(cullPendingSerial_.begin(), cullPendingSerial_.end(), 0u);
        std::fill(slotChangeSerial_.begin(), slotChangeSerial_.end(), 0u);
        changedSinceVerdict_.clear();
        verdictVisible_.clear();
        haveVerdict_ = false;
        slots.markAllChanged(InstanceSlots::Mirror::GpuCull);
    }
    const uint32_t slotCount = slots.slotCount();
    if (cullPendingSerial_.size() < slotCount) cullPendingSerial_.resize(slotCount, 0u);
    if (slotChangeSerial_.size() < slotCount) slotChangeSerial_.resize(slotCount, 0u);
    slots.catchUp(InstanceSlots::Mirror::GpuCull, [&](uint32_t slot, uint32_t) {
        if (cullPendingSerial_[slot] == 0) cullPendingSlots_.push_back(slot);
        cullPendingSerial_[slot] = serial;
        slotChangeSerial_[slot] = serial;
        changedSinceVerdict_.push_back(slot);
    });

    // What a submitted frame carried is on the GPU; the rest rides again, as
    // it stands now.
    const uint32_t applied = cullAppliedSerial_.load(std::memory_order_acquire);
    std::erase_if(cullPendingSlots_, [&](uint32_t slot) {
        if (cullPendingSerial_[slot] > applied) return false;
        cullPendingSerial_[slot] = 0;
        return true;
    });
    frame.cullReset = cullResetSerial_ > applied;
    frame.cullUpdateSlots.reserve(cullPendingSlots_.size());
    frame.cullUpdateRows.reserve(cullPendingSlots_.size());
    frame.instanceUpdateRows.reserve(cullPendingSlots_.size());
    for (const uint32_t slot : cullPendingSlots_) {
        frame.cullUpdateSlots.push_back(slot);
        if (slots.idAt(slot) == InstanceSlots::invalid) {
            // A free slot is never drawn; its cull row keeps the cull off it.
            frame.cullUpdateRows.push_back(CullRowGPU{});
            frame.instanceUpdateRows.push_back(InstanceStaticGPU{});
            continue;
        }
        const size_t index = scene_->indexOfSlot[slot];
        const M2Instance& instance = scene_->instances[index];
        frame.cullUpdateRows.push_back(cullRowOf(index));
        frame.instanceUpdateRows.push_back(
            InstanceStaticGPU{.model = instance.modelMatrix, .interiorLight = instance.interiorLight});
    }
    frame.cullSlotCount = slotCount;

    // The material rows ride the same way: from the first frame extracted
    // after they were written until one that carried them is on the GPU.
    MaterialRows& rows = materialRows_;
    std::erase_if(rows.pending, [&](uint32_t row) {
        uint32_t& carried = rows.pendingSerial[row];
        if (carried == kMaterialRowUncarried) carried = serial;
        if (carried > applied) return false;
        carried = 0;
        return true;
    });
    frame.materialUpdateSlots.assign(rows.pending.begin(), rows.pending.end());
    frame.materialUpdateRows.reserve(rows.pending.size());
    for (const uint32_t row : rows.pending) frame.materialUpdateRows.push_back(rows.data[row]);
    frame.materialRowCount = static_cast<uint32_t>(rows.data.size());
}

M2Renderer::CullRowGPU M2Renderer::cullRowOf(size_t index) const {
    const M2CullGrid::Record& r = scene_->cullGrid_.record(static_cast<uint32_t>(index));
    return {.sphere = glm::vec4(r.cullCenter, r.paddedRadius), .maxDistSqFactor = r.maxDistSqFactor,
            .flags = r.flags, .id = r.id, .centreOffset = glm::length(r.cullCenter - r.position)};
}

namespace {

uint32_t grownCapacity(uint32_t capacity, uint32_t need) {
    return std::max({need, capacity * 2u, 1024u});
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    cmdPipelineBarrier2(cmd, dep);
}

}  // namespace

bool M2Renderer::makeCullBuffer(CullBuffer& into, uint32_t capacity, VkDeviceSize bytes, VkBufferUsageFlags usage,
                                VmaMemoryUsage memory, const char* what) {
    VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = memory;
    if (memory != VMA_MEMORY_USAGE_GPU_ONLY) aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    CullBuffer next;
    if (gpuCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &next.buffer, &next.alloc, &info) != VK_SUCCESS) {
        static core::LogBudget budget(4, "M2 cull buffers");
        if (budget.take())
            LOG_ERROR("M2 slot tables: no memory for the ", what, " of ", capacity,
                      " slots; this frame's models are not drawn, or culled on the CPU");
        return false;
    }
    next.mapped = info.pMappedData;
    next.capacity = capacity;
    DeferredResources retire(*vkCtx_);
    if (into.buffer) retire.buffer(into.buffer, into.alloc);
    into = next;
    return true;
}

bool M2Renderer::ensureSlotTables(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t slots, uint32_t materialRows) {
    if (!cullTable_.reserve(cmd, slots) || !instanceTable_.reserve(cmd, slots) ||
        !materialTable_.reserve(cmd, std::max(materialRows, 1u))) {
        return false;
    }
    // The view's set names the material table too, by the same rule.
    if (instanceSet_[frameIndex] && instanceSetMaterialVersion_[frameIndex] != materialTable_.version()) {
        const VkDescriptorBufferInfo materials{.buffer = materialTable_.buffer(), .offset = 0,
                                               .range = materialTable_.bytes()};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = instanceSet_[frameIndex];
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &materials;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
        instanceSetMaterialVersion_[frameIndex] = materialTable_.version();
    }
    // This slot's sets - the view's and the shadow maps' - are written after
    // its fence, before either pass binds them, and only when the table they
    // name was replaced.
    const VkDescriptorBufferInfo table{.buffer = instanceTable_.buffer(), .offset = 0,
                                       .range = instanceTable_.bytes()};
    const auto nameTable = [&](VkDescriptorSet set, uint32_t& version) {
        if (!set || version == instanceTable_.version()) return;
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &table;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
        version = instanceTable_.version();
    };
    nameTable(instanceSet_[frameIndex], instanceSetTableVersion_[frameIndex]);
    nameTable(shadowInstanceSet_[frameIndex], shadowSetTableVersion_[frameIndex]);
    return true;
}

bool M2Renderer::ensureCullOutput(uint32_t frameIndex, uint32_t slots) {
    bool visibleReplaced = false;
    if (slots > cullVisible_[frameIndex].capacity) {
        const uint32_t capacity = grownCapacity(cullVisible_[frameIndex].capacity, slots);
        if (!makeCullBuffer(cullVisible_[frameIndex], capacity, sizeof(uint32_t) * (1 + 2 * VkDeviceSize(capacity)),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_GPU_TO_CPU, "kept list")) {
            return false;
        }
        visibleReplaced = true;
    }
    // This slot's set is written after its fence, and only when what it
    // names was replaced.
    if (visibleReplaced || cullSetTableVersion_[frameIndex] != cullTable_.version()) {
        const VkDescriptorBufferInfo table{.buffer = cullTable_.buffer(), .offset = 0, .range = cullTable_.bytes()};
        const VkDescriptorBufferInfo kept{
            .buffer = cullVisible_[frameIndex].buffer, .offset = 0,
            .range = sizeof(uint32_t) * (1 + 2 * VkDeviceSize(cullVisible_[frameIndex].capacity))};
        VkWriteDescriptorSet writes[2] = {};
        for (int w = 0; w < 2; ++w) {
            writes[w] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[w].dstSet = cullSet_[frameIndex];
            writes[w].dstBinding = static_cast<uint32_t>(w + 1);
            writes[w].descriptorCount = 1;
            writes[w].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        writes[0].pBufferInfo = &table;
        writes[1].pBufferInfo = &kept;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
        cullSetTableVersion_[frameIndex] = cullTable_.version();
    }
    return true;
}

void M2Renderer::frameSubmitted() {
    if (pendingUploadSerial_ != 0) {
        cullAppliedSerial_.store(pendingUploadSerial_, std::memory_order_release);
        pendingUploadSerial_ = 0;
    }
    if (!pendingCullDispatch_.valid) return;
    slotCullDispatch_[pendingCullDispatch_.frameIndex] = pendingCullDispatch_;
    pendingCullDispatch_ = CullDispatch{};
}

void M2Renderer::uploadSlotTables(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!recording_) return;
    const Frame& frame = *recording_;
    if (frame.cullSlotCount == 0) return;
    if (!ensureSlotTables(cmd, frameIndex, frame.cullSlotCount, frame.materialRowCount)) return;

    // A free instance row is never read: only the cull's rows need clearing.
    if (frame.cullReset) cullTable_.clear(cmd);
    if (!cullTable_.write(cmd, frameIndex, frame.cullUpdateSlots, frame.cullUpdateRows.data()) ||
        !instanceTable_.write(cmd, frameIndex, frame.cullUpdateSlots, frame.instanceUpdateRows.data()) ||
        !materialTable_.write(cmd, frameIndex, frame.materialUpdateSlots, frame.materialUpdateRows.data())) {
        // Not acknowledged, so the same slots come again with the next frame.
        return;
    }
    pendingUploadSerial_ = frame.cullSerial;
}

void M2Renderer::dispatchCullCompute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!cullPipeline_) return;
    // What this slot's last submitted cull kept, now its fence has passed.
    collectCullOutput(frameIndex);
    const Frame& frame = *recording_;
    if (frame.cullSlotCount == 0 || !cullUniformMapped_[frameIndex]) return;
    // The table as this frame left it, or no cull (uploadSlotTables said why).
    if (pendingUploadSerial_ != frame.cullSerial) return;
    if (!ensureCullOutput(frameIndex, frame.cullSlotCount)) return;

    // The count starts at zero after the last frame's cull wrote it.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, cullVisible_[frameIndex].buffer, 0, sizeof(uint32_t), 0);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    // The guard of the extracted frame's view, so the verdict holds for the
    // frames after it whose view the guard still covers.
    const CullGuard& guard = frame.cullGuard;
    auto* ubo = static_cast<CullUniformsGPU*>(cullUniformMapped_[frameIndex]);
    for (size_t i = 0; i < guard.planes.size(); ++i) ubo->guardPlanes[i] = guard.planes[i];
    ubo->cameraPos = glm::vec4(guard.position, frame.maxPossibleDistSq);
    ubo->slotCount = frame.cullSlotCount;
    ubo->baseMaxDistSq = frame.cullBaseMaxDistSq;
    ubo->viewDistance = frame.cullViewDistance;
    ubo->groundDetailMaxDistance = frame.cullGroundDetailMaxDistance;
    ubo->gameObjectMinDistance = rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE;
    ubo->skyBirdMaxDistance = rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
    ubo->guardDistance = kCullGuardDistance;
    ubo->rangeSlack = kCullGuardRangeSlack;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cullPipelineLayout_, 0, 1, &cullSet_[frameIndex], 0, nullptr);
    vkCmdDispatch(cmd, (frame.cullSlotCount + 63) / 64, 1, 1);
    pendingCullDispatch_ = CullDispatch{.valid = true, .frameIndex = frameIndex,
                                        .serial = frame.cullSerial, .sceneIdentity = frame.sceneIdentity,
                                        .guard = guard,
                                        .ranges = {std::sqrt(frame.cullBaseMaxDistSq), frame.cullViewDistance,
                                                   frame.cullGroundDetailMaxDistance}};

    // The kept list is the host's after this frame's fence: collectCullOutput
    // invalidates and reads it when this frame slot comes round again.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

namespace {

// A call's first draw record: draw gl_DrawID of the call reads its material
// row from drawMaterials[drawBase + gl_DrawID] (m2.vert).
struct M2PushConstants {
    uint32_t drawBase;
};
static_assert(sizeof(M2PushConstants) == kM2PushConstantBytes, "m2.vert's Push block");

// Draws that follow one another with the same pipeline, faces and geometry
// page, recorded as one indirect call. Each draw is written in order into
// the frame slot's command and record buffers, so a call draws exactly what
// its draws one by one did, in the same order; the caller flushes before
// it changes any of that state. A call's draws read their records from the
// first one on by gl_DrawID; `announce(first)` tells the shader which.
template <class Record, class Announce>
class DrawRuns {
public:
    DrawRuns(VkCommandBuffer cmd, ::VkBuffer commands, void* commandData, void* recordData, uint32_t& cursor,
             uint32_t capacity, uint32_t maxPerCall, Announce announce)
        : cmd_(cmd), commands_(commands), commandData_(static_cast<VkDrawIndexedIndirectCommand*>(commandData)),
          recordData_(static_cast<Record*>(recordData)), cursor_(cursor), first_(cursor), capacity_(capacity),
          maxPerCall_(std::max(1u, maxPerCall)), announce_(std::move(announce)) {}
    /// The draw added to the run; false when the frame slot's room is used up.
    bool add(const VkDrawIndexedIndirectCommand& draw, const Record& record) {
        if (cursor_ >= capacity_) return false;
        commandData_[cursor_] = draw;
        recordData_[cursor_] = record;
        ++cursor_;
        if (cursor_ - first_ >= maxPerCall_) flush();
        return true;
    }
    /// The run so far recorded as one call.
    void flush() {
        const uint32_t count = cursor_ - first_;
        if (count == 0) return;
        announce_(first_);
        vkCmdDrawIndexedIndirect(cmd_, commands_, VkDeviceSize(first_) * sizeof(VkDrawIndexedIndirectCommand), count,
                                 sizeof(VkDrawIndexedIndirectCommand));
        first_ = cursor_;
    }

private:
    VkCommandBuffer cmd_;
    ::VkBuffer commands_;
    VkDrawIndexedIndirectCommand* commandData_;
    Record* recordData_;
    uint32_t& cursor_;
    uint32_t first_;
    uint32_t capacity_;
    uint32_t maxPerCall_;
    Announce announce_;
};

/// The view's runs: a material row a draw, the first pushed.
auto viewRuns(VkCommandBuffer cmd, VkPipelineLayout layout, const AllocatedBuffer& commands,
              const AllocatedBuffer& records, uint32_t& cursor, uint32_t capacity, uint32_t maxPerCall) {
    return DrawRuns<uint32_t, std::function<void(uint32_t)>>(
        cmd, commands.buffer, commands.info.pMappedData, records.info.pMappedData, cursor, capacity, maxPerCall,
        [cmd, layout](uint32_t first) {
            const M2PushConstants push{.drawBase = first};
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        });
}

/// Said once a stretch when a frame slot's draws outgrow its room.
void sayDrawRoomFull(uint32_t capacity) {
    static core::LogBudget budget(4, "M2 draw room full");
    if (budget.take())
        LOG_WARNING("M2Renderer: this frame's ", capacity, " draws filled the frame slot's command room - the rest "
                    "of its models are not drawn this frame");
}

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

uint8_t M2Renderer::drawVariant(const M2ModelGPU::BatchGPU& batch) const {
    const M2BatchDrawState& state = batch.drawState;
    if (skyMode_ && state.drawBlendMode <= 1) return kVariantSkyAlpha;
    if (state.forceCutout) return kVariantCutout;
    return static_cast<uint8_t>(m2BlendPipeline(state.drawBlendMode));
}

VkPipeline M2Renderer::variantPipeline(uint8_t variant) const {
    if (variant == kVariantSkyAlpha) return scenePipes_.alpha;
    if (variant == kVariantCutout) return cutoutPipeline_;
    return blendPipeline(static_cast<M2BlendPipeline>(variant));
}

void M2Renderer::buildDrawList(M2ModelGPU& model) {
    skeletons_.release(std::exchange(model.drawList, {}));
    for (auto& level : model.levelDraws) level = {};
    if (!model.geometry.placed()) return;

    // What each batch of a named model does in the view (WOWEE_M2_BATCH_DIAG).
    const bool diag = m2BatchDiagNames(model);
    std::vector<uint32_t> words(kDrawListHeaderWords, 0u);
    std::array<std::vector<uint32_t>, kDrawListLevels> own;
    bool any = false;
    for (uint32_t lod = 0; lod < kDrawListLevels; ++lod) {
        M2ModelGPU::LevelDraws& level = model.levelDraws[lod];
        words[lod * 4] = static_cast<uint32_t>(words.size());
        uint16_t phase = 0;
        uint16_t lastState = 0;
        for (size_t bi = 0; bi < model.batches.size(); ++bi) {
            const auto& batch = model.batches[bi];
            // The baked stars are in the list; the passes leave them out
            // where the client draws its own.
            const char* skipped = opaqueBatchSkip(model, bi, static_cast<uint16_t>(lod), diag, false);
            const M2GlowCardBatch glowCard = glowCardOf(model, batch);
            // A glow card's sprites are the cull's (appendGlowSprites); the
            // card itself is left out where the sprite stands in for it. A
            // layer the client blends for its alpha is the blended pass's.
            if (!skipped && m2WantsGlowSprite(glowCard) && m2GlowSpriteReplacesMesh(glowCard))
                skipped = "replaced by its glow sprite";
            if (!skipped && (batch.blendMode >= 2 || batch.blendedByOpacity)) skipped = "drawn in the blended pass";
            if (!skipped && batch.materialRow == kNoMaterialRow) skipped = "no material row";
            if (diag)
                LOG_WARNING("M2 DRAW '", model.name, "' batch ", bi, " lod=", lod,
                            " submeshLevel=", batch.submeshLevel, " idx=", batch.indexCount,
                            " opacity=", batch.batchOpacity,
                            (skipped ? "  SKIPPED: " : "  drawn"), (skipped ? skipped : ""));
            if (skipped) continue;

            const uint8_t variant = drawVariant(batch);
            const bool twoSided = skyMode_ || m2MaterialTwoSided(batch.materialFlags);
            const auto entryIndex = static_cast<uint32_t>(level.entries.size());
            // Its bucket among the level's: in the world by its phase - how
            // many times the state changed before it in the model's order -
            // its pipeline and its faces. The buckets are drawn phase by
            // phase, so a model's batches keep its own order: a cut-out
            // layer before an opaque one at the same depth stays before it.
            // In the sky, whose layers blend, a bucket an entry.
            uint32_t bucket = 0;
            uint32_t ordinal = 0;
            const auto state = static_cast<uint16_t>((variant << 1) | (twoSided ? 1 : 0));
            if (!level.entries.empty() && state != lastState) ++phase;
            lastState = state;
            const auto key = static_cast<uint16_t>((phase << kDrawPhaseShift) | state);
            if (skyMode_) {
                bucket = static_cast<uint32_t>(level.buckets.size());
                level.buckets.push_back(key);
                level.bucketCounts.push_back(1);
            } else {
                const auto found = std::find(level.buckets.begin(), level.buckets.end(), key);
                bucket = static_cast<uint32_t>(found - level.buckets.begin());
                if (found == level.buckets.end()) {
                    level.buckets.push_back(key);
                    level.bucketCounts.push_back(0);
                }
                ordinal = level.bucketCounts[bucket]++;
            }

            // Records of its own where its instances differ per batch: a
            // texture animation, the batch's own opacity, the sky's of the
            // hour.
            // Diagnostic: WOWEE_M2_NO_TEXTURE_ANIMATION=1 holds every view
            // texture still, which tells an animation fault from anything else.
            static const bool kNoTextureAnimation = std::getenv("WOWEE_M2_NO_TEXTURE_ANIMATION") != nullptr;
            const bool animated =
                !kNoTextureAnimation && batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation;
            const bool ownRecords = animated || skyMode_ || batch.batchOpacity != 1.0f;
            const auto transformAt = [&](size_t lookupIndex) -> uint32_t {
                if (!animated || !model.textureAnimations.placed()) return ~0u;
                if (lookupIndex >= model.textureTransformLookup.size()) return ~0u;
                const uint16_t index = model.textureTransformLookup[lookupIndex];
                return index < model.textureTransforms.size() ? index : ~0u;
            };
            const uint32_t uv0 = transformAt(batch.textureAnimIndex);
            const uint32_t uv1 = batch.secondTexture ? transformAt(size_t(batch.textureAnimIndex) + 1) : ~0u;
            const uint32_t ownOrdinal = ownRecords ? static_cast<uint32_t>(own[lod].size()) : 0u;
            if (ownRecords) own[lod].push_back(entryIndex);
            words.insert(words.end(),
                         {batch.indexCount, model.geometry.firstIndex + batch.indexStart,
                          static_cast<uint32_t>(model.geometry.baseVertex), batch.materialRow, bucket, ordinal, uv0,
                          uv1, std::bit_cast<uint32_t>(batch.batchOpacity),
                          ownRecords ? kDrawEntryOwnRecords : 0u, ownOrdinal, 0u});
            level.entries.push_back({.batch = static_cast<uint16_t>(bi),
                                     .variant = variant,
                                     .twoSided = twoSided,
                                     .starLayer = batch.starLayer,
                                     .replacesStars = batch.starLayer && batch.blendMode <= 1 &&
                                                      !batch.blendedByOpacity});
            any = true;
        }
        words[lod * 4 + 1] = static_cast<uint32_t>(level.entries.size());
        level.ownCount = static_cast<uint32_t>(own[lod].size());
    }
    if (!any) return;
    for (uint32_t lod = 0; lod < kDrawListLevels; ++lod) {
        words[lod * 4 + 2] = static_cast<uint32_t>(words.size());
        words[lod * 4 + 3] = static_cast<uint32_t>(own[lod].size());
        words.insert(words.end(), own[lod].begin(), own[lod].end());
    }
    vkCtx_->beginUploadBatch();
    model.drawList = skeletons_.placeBlob(words, model.name.c_str(), "not drawn in the view");
    vkCtx_->endUploadBatch();
    if (!model.drawList.placed())
        for (auto& level : model.levelDraws) level = {};
}

void M2Renderer::appendGlowSprites(const M2Instance& inst, const M2ModelGPU& model, uint16_t lod,
                                   const glm::vec3& camPos,
                                   std::vector<GlowSprite>& out, float time) const {
    // The groups the opaque pass leaves out altogether.
    if (skipGroundDetail_ && model.isGroundDetail) return;
    if (!model.geometry.placed()) return;
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
        // Glow sprites share their parent model's cull range. Atmosphere
        // attenuates them in the shader, without a second distance fade.
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


void M2Renderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera&,
                        const StarReplacement& drawStars, bool drawBakedStars) {
    if (!opaquePipeline_) return;
    recordView(cmd, perFrameSet, drawStars, drawBakedStars);
}


void M2Renderer::joinVisibleChunks(Frame& frame) {
    auto& chunks = visibilityChunks_;
    posedThisFrame_.clear();
    const auto reserveGrowing = [](auto& values, size_t count) {
        if (count > values.capacity())
            values.reserve(std::max(count, values.capacity() + values.capacity() / 2));
    };
    size_t drawCount = 0, opaqueCount = 0, transparentCount = 0, glowCount = 0;
    for (const auto& chunk : chunks) {
        drawCount += chunk.draws.size(); opaqueCount += chunk.opaque.size();
        transparentCount += chunk.transparent.size(); glowCount += chunk.glows.size();
    }
    reserveGrowing(frame.drawInstances_, drawCount);
    reserveGrowing(frame.sortedVisible_, opaqueCount);
    reserveGrowing(frame.transparentVisible_, transparentCount);
    reserveGrowing(frame.glowSprites_, glowCount);

    // The kept skinned draws are posed at their own bone count, so a 300-bone
    // creature gets all 300 matrices.
    SkeletonFrame& skeleton = frame.skeleton;
    for (auto& chunk : chunks) {
        // Each chunk numbered its draws from zero; they follow the ones
        // before it here.
        const uint32_t base = static_cast<uint32_t>(frame.drawInstances_.size());
        // And its bones from zero, after the ones before it.
        const uint32_t boneBase = skeleton.boneCount;
        const auto requestBase = static_cast<uint32_t>(skeleton.requests.size());
        for (const auto& [index, request] : chunk.posed) posedThisFrame_[index] = requestBase + request;
        for (const uint32_t d : chunk.skinned) chunk.draws[d].boneBase += boneBase;
        frame.skinnedDraws += static_cast<uint32_t>(chunk.skinned.size());
        for (SkeletonRequestGPU& request : chunk.requests) request.outBase += boneBase;
        for (SkeletonFrame::Read& read : chunk.reads) read.source += boneBase;
        skeleton.requests.insert(skeleton.requests.end(), chunk.requests.begin(), chunk.requests.end());
        skeleton.reads.insert(skeleton.reads.end(), chunk.reads.begin(), chunk.reads.end());
        skeleton.boneCount += chunk.boneCount;
        frame.drawInstances_.insert(frame.drawInstances_.end(), std::make_move_iterator(chunk.draws.begin()),
                                    std::make_move_iterator(chunk.draws.end()));
        for (VisibleEntry& e : chunk.opaque) e.draw += base;
        for (VisibleEntry& e : chunk.transparent) e.draw += base;
        frame.sortedVisible_.insert(frame.sortedVisible_.end(), chunk.opaque.begin(), chunk.opaque.end());
        frame.transparentVisible_.insert(frame.transparentVisible_.end(), chunk.transparent.begin(),
                                         chunk.transparent.end());
        frame.glowSprites_.insert(frame.glowSprites_.end(), chunk.glows.begin(), chunk.glows.end());
        chunk.clear();
    }
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
        uint32_t totalValid = 0, totalAnimated = 0, totalBonesRead = 0;
        for (const auto& inst : scene_->instances) {
            if (inst.cachedIsValid) totalValid++;
            if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
                totalAnimated++;
                if (!inst.boneMatrices.empty()) totalBonesRead++;
            }
        }
        LOG_INFO("M2 diag: total=", scene_->instances.size(),
                 " valid=", totalValid,
                 " animated=", totalAnimated,
                 " bonesRead=", totalBonesRead,
                 " visible=", sortedVisible_.size(),
                 " draws=", lastDrawCallCount);
    }

    // Reuse persistent buffers (clear instead of reallocating)
    glowSprites_.clear();
    drawInstances_.clear();


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
        for (size_t index = 0; index < scene_->instances.size(); ++index) {
            const auto& inst = scene_->instances[index];
            if (!inst.cachedModel || !m2BatchDiagNames(*inst.cachedModel)) continue;
            const bool lastCullVisible = scene_->cullGrid_.record(static_cast<uint32_t>(index)).lastVisible != 0;
            const float distanceSq=glm::dot(inst.position-camPos,inst.position-camPos);
            const float centreSq=glm::dot(inst.cachedCullCenter-camPos,inst.cachedCullCenter-camPos);
            const float effective=m2InstanceMaxDistSq(maxRenderDistanceSq,inst.cachedEffectiveMaxDistSqFactor,
                inst.isGameObject,M2_GAME_OBJECT_MIN_RENDER_DISTANCE,cappedViewDistance(),
                inst.cachedIsGroundDetail,groundDetailMaxDistance_);
            const bool inFrustum=frustum.intersectsSphere(inst.cachedCullCenter,inst.cachedPaddedRadius);
            const uint32_t reason=(distanceSq>maxPossibleDistSq ? 1u:0u) |
                (centreSq>maxPossibleDistSq ? 2u:0u) | (distanceSq>effective ? 4u:0u) |
                (!inFrustum ? 8u:0u) | (!lastCullVisible ? 16u:0u) | (inst.hidden ? 32u:0u) |
                (!inst.cachedIsValid ? 64u:0u) | (inst.cachedIsInvisibleTrap ? 128u:0u);
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
                " bones=",inst.boneMatrices.size());
        }
    }

    // What the GPU's last verdict kept and what changed since it, while the
    // verdict's guard covers this view; otherwise the columns in view. This
    // walked the whole resident world - ninety thousand instances in Dalaran
    // - for the few thousand it drew, and then the columns in view - some
    // twenty thousand.
    if (!forceNoCull_) {
        frame.culledFromColumns = gatherCullCandidates(
            camera, frustum, maxPossibleDistSq,
            {smoothedRenderDist_, cappedViewDistance(), groundDetailMaxDistance_});
    }
    const auto& candidates = forceNoCull_ ? viewCandidates_ : cullCandidates_;
    if (forceNoCull_) {
        viewCandidates_.resize(scene_->instances.size());
        for (uint32_t i = 0; i < viewCandidates_.size(); ++i) viewCandidates_[i] = i;
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
        using G = M2CullGrid;
        const G& grid = scene_->cullGrid_;
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t i = list[k];
            // The tests read the instance's cull record; the instance itself
            // only once it is kept.
            const G::Record& r = grid.record(i);
            // A finished effect shows only the particles it has left. A
            // hidden one is in no column; this keeps it out of the walks
            // over every instance (forceNoCull_, the cull check) as well.
            if (r.flags & (G::kFinishing | G::kHidden)) continue;
            float distSq;
            float effectiveMaxDistSq;

            // Server game objects keep a distance floor instead of following
            // the ambient doodad range down. The final range only culls geometry;
            // atmosphere handles the visible attenuation inside it.
            const float instanceMaxDistSq = rendering::m2InstanceMaxDistSq(
                maxRenderDistanceSq, r.maxDistSqFactor,
                (r.flags & G::kGameObject) != 0, rendering::M2_GAME_OBJECT_MIN_RENDER_DISTANCE,
                cappedViewDistance(),
                (r.flags & G::kGroundDetail) != 0, groundDetailMaxDistance_);

            if (forceNoCull_) {
                if (!(r.flags & G::kValid)) continue;
                glm::vec3 toCam = r.position - camPos;
                distSq = glm::dot(toCam, toCam);
                effectiveMaxDistSq = instanceMaxDistSq;
            } else {
                if (!(r.flags & G::kValid) || (r.flags & G::kInvisibleTrap)) continue;
                glm::vec3 toCam = r.position - camPos;
                distSq = glm::dot(toCam, toCam);
                if (distSq > maxPossibleDistSq) continue;
                effectiveMaxDistSq = instanceMaxDistSq;
                if (distSq > effectiveMaxDistSq) continue;
                if (r.paddedRadius > 0.0f && !frustum.intersectsSphere(r.cullCenter, r.paddedRadius)) continue;
            }

            if (r.flags & G::kAnimatedSkyBird) {
                constexpr float kBirdMaxDistSq =
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE *
                    rendering::M2_SKY_BIRD_MAX_RENDER_DISTANCE;
                effectiveMaxDistSq = std::min(effectiveMaxDistSq, kBirdMaxDistSq);
                if (distSq > effectiveMaxDistSq) continue;
            }
            const auto& instance = scene_->instances[i];

            // A faded instance is drawn blended, all of it - see M2Instance::fade.
            const bool faded = instance.fade < 0.999f;
            const bool transparent = faded || (instance.cachedModel &&
                                               instance.cachedModel->hasTransparentBatches);
            // What the passes will draw of it, so they need not come back here.
            const uint32_t draw = static_cast<uint32_t>(out.draws.size());
            // Its skeleton is posed on the GPU because it is drawn, and the
            // bones the CPU reads of it are copied back.
            const bool skinned = instance.cachedHasAnimation && !instance.cachedDisableAnimation &&
                                 instance.cachedModel && instance.cachedModel->skeleton.placed();
            const uint32_t firstBone = out.boneCount;
            if (skinned) {
                if (instance.cachedHasEmitters || instance.cachedModel->canThrowLocalLight())
                    out.posed.emplace_back(i, static_cast<uint32_t>(out.requests.size()));
                out.requests.push_back(skeletonRequest(instance, firstBone));
                for (const uint16_t bone : instance.cachedModel->readBones)
                    out.reads.push_back({.owner = instance.id, .bone = bone, .source = firstBone + bone});
                out.boneCount += instance.cachedModel->skeleton.boneCount;
                out.skinned.push_back(draw);
            }
            out.draws.push_back(DrawInstance{
                .slot = r.slot,
                .model = models.at(instance.modelId),
                .localLights = haveLights
                    ? pickLocalLights(*frameLights_, glm::vec3(instance.modelMatrix[3]), 0.0f)
                    : kNoLocalLights,
                .position = instance.position,
                .scale = instance.scale,
                .fade = instance.fade,
                .highlight = instance.highlight,
                .animTime = animTimeOf(instance),
                .globalSequenceTime = globalSequenceTimeOf(instance),
                .sequenceIndex = instance.currentSequenceIndex,
                .boneBase = skinned ? firstBone : 0,
                .boneCount = skinned ? instance.cachedModel->skeleton.boneCount : 0,
                .afloat = instance.afloat > 0,
            });
            VisibleEntry visible{.draw = draw, .modelId = instance.modelId, .distSq = distSq};
            if (!faded) {
                out.opaque.push_back(visible);
                // On its bones as last read back; at rest until the first.
                if (instance.cachedModel && instance.cachedModel->hasGlowSprites) {
                    const M2ModelGPU& model = *instance.cachedModel;
                    appendGlowSprites(instance, model, m2LodFor(model, distSq), camPos, out.glows, frame.time);
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
    joinVisibleChunks(frame);

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
                    " candidates=",candidates.size()," bones=",inst.boneMatrices.size());
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
    // Pass 2 is drawn back to front, one instance at a time. Equally far ones
    // by where they stand: the visible list follows the order the instances
    // were loaded in, which differs from one run to the next.
    std::sort(transparentVisible_.begin(), transparentVisible_.end(),
              [&](const VisibleEntry& a, const VisibleEntry& b) {
                  if (a.distSq != b.distSq) return a.distSq > b.distSq;
                  const glm::vec3& p = drawInstances_[a.draw].position;
                  const glm::vec3& q = drawInstances_[b.draw].position;
                  return std::tie(p.x, p.y, p.z) < std::tie(q.x, q.y, q.z);
              });
    // The glows far to near too: blended in load order, the same lamps summed
    // to a different value from one run to the next.
    const glm::vec3 eye = camera.getPosition();
    std::sort(glowSprites_.begin(), glowSprites_.end(), [&](const GlowSprite& a, const GlowSprite& b) {
        const float da = glm::dot(a.worldPos - eye, a.worldPos - eye);
        const float db = glm::dot(b.worldPos - eye, b.worldPos - eye);
        if (da != db) return da > db;
        const glm::vec3& p = a.worldPos;
        const glm::vec3& q = b.worldPos;
        return std::tie(p.x, p.y, p.z) < std::tie(q.x, q.y, q.z);
    });
}
void M2Renderer::collectCullOutput(uint32_t frame) {
    CullDispatch& done = slotCullDispatch_[frame];
    if (!done.valid) return;
    done.valid = false;
    const CullBuffer& list = cullVisible_[frame];
    if (!list.mapped) return;
    vmaInvalidateAllocation(vkCtx_->getAllocator(), list.alloc, 0, VK_WHOLE_SIZE);
    const auto* data = static_cast<const uint32_t*>(list.mapped);
    const uint32_t count = std::min(data[0], list.capacity);
    CullVerdict verdict{.sceneIdentity = done.sceneIdentity, .serial = done.serial, .guard = done.guard,
                        .ranges = done.ranges};
    verdict.kept.assign(data + 1, data + 1 + 2 * size_t(count));
    std::lock_guard<std::mutex> lock(cullResultMutex_);
    collectedCull_.push_back(std::move(verdict));
}

std::vector<CullVerdict> M2Renderer::takeCullVerdicts() {
    std::lock_guard lock(cullResultMutex_);
    std::vector<CullVerdict> result;
    result.swap(collectedCull_);
    return result;
}

void M2Renderer::acceptCullVerdicts(const std::vector<CullVerdict>& verdicts) {
    const CullVerdict* newest = nullptr;
    for (const auto& verdict : verdicts) {
        if (verdict.sceneIdentity != scene_->identity) continue;
        if (!newest || verdict.serial > newest->serial) newest = &verdict;
    }
    if (!newest) return;
    if (haveVerdict_ && verdictScene_ == scene_->identity && newest->serial <= verdictSerial_) return;
    const InstanceSlots& slots = scene_->slots;
    const uint32_t slotCount = slots.slotCount();
    if (slotMark_.size() < slotCount) slotMark_.resize(slotCount, 0u);
    if (slotChangeSerial_.size() < slotCount) slotChangeSerial_.resize(slotCount, 0u);
    if (++markStamp_ == 0) {
        std::fill(slotMark_.begin(), slotMark_.end(), 0u);
        markStamp_ = 1;
    }
    const uint32_t stamp = markStamp_;
    const uint32_t serial = newest->serial;
    auto& grid = scene_->cullGrid_;
    // A slot the verdict covers: its instance unchanged since that frame.
    const auto covered = [&](uint32_t slot) {
        return slot < slotCount && slots.idAt(slot) != InstanceSlots::invalid && slotChangeSerial_[slot] <= serial;
    };
    const auto setVisible = [&](uint32_t slot, uint8_t visible) {
        const uint32_t index = scene_->indexOfSlot[slot];
        if (index < grid.size()) grid.record(index).lastVisible = visible;
    };

    std::vector<uint32_t> kept;
    kept.reserve(newest->kept.size() / 2);
    for (size_t k = 0; k + 1 < newest->kept.size(); k += 2) {
        const uint32_t slot = newest->kept[k];
        if (!covered(slot) || slots.idAt(slot) != newest->kept[k + 1]) continue;
        slotMark_[slot] = stamp;
        kept.push_back(slot);
        setVisible(slot, 1);
    }
    // Kept no longer: what the last verdict kept, and what changed since it
    // and is covered now, unless this one kept it too.
    for (const uint32_t slot : verdictVisible_) {
        if (covered(slot) && slotMark_[slot] != stamp) setVisible(slot, 0);
    }
    std::erase_if(changedSinceVerdict_, [&](uint32_t slot) {
        if (slot < slotCount && slotChangeSerial_[slot] > serial) return false;
        if (covered(slot) && slotMark_[slot] != stamp) setVisible(slot, 0);
        return true;
    });
    // The same order whatever order the GPU appended them in.
    std::sort(kept.begin(), kept.end());
    verdictVisible_ = std::move(kept);
    verdictSerial_ = serial;
    verdictGuard_ = newest->guard;
    verdictRanges_ = newest->ranges;
    verdictScene_ = scene_->identity;
    haveVerdict_ = true;
}

bool M2Renderer::gatherCullCandidates(const Camera& camera, const Frustum& frustum, float maxPossibleDistSq,
                                      const std::array<float, 3>& ranges) {
    auto& out = cullCandidates_;
    out.clear();
    // The verdict holds only while its guard covers this view and its ranges
    // these: otherwise something in view now may lie outside what it tested.
    // WOWEE_CULL_ON_CPU=1 never uses one: the picture a GPU verdict should
    // give, to compare it with.
    bool covered = !cullOnCpu() && haveVerdict_ && verdictScene_ == scene_->identity &&
                   cullGuardCovers(verdictGuard_, camera);
    for (size_t i = 0; covered && i < ranges.size(); ++i) {
        // A ground clutter range of 0 is none at all, wider than any.
        const bool unbounded = ranges[i] <= 0.0f, wasUnbounded = verdictRanges_[i] <= 0.0f;
        covered = wasUnbounded || (!unbounded && cullGuardRangeCovers(verdictRanges_[i], ranges[i]));
    }
    if (!covered) {
        // No verdict that holds: the columns in view, tested here.
        gatherViewCandidates(camera.getPosition(), frustum, maxPossibleDistSq, out);
        return true;
    }
    const InstanceSlots& slots = scene_->slots;
    const uint32_t slotCount = slots.slotCount();
    if (slotMark_.size() < slotCount) slotMark_.resize(slotCount, 0u);
    if (++markStamp_ == 0) {
        std::fill(slotMark_.begin(), slotMark_.end(), 0u);
        markStamp_ = 1;
    }
    const uint32_t stamp = markStamp_;
    out.reserve(verdictVisible_.size() + changedSinceVerdict_.size());
    // What the GPU kept inside the guard, unchanged since.
    for (const uint32_t slot : verdictVisible_) {
        if (slots.idAt(slot) == InstanceSlots::invalid || slotChangeSerial_[slot] > verdictSerial_) continue;
        slotMark_[slot] = stamp;
        out.push_back(scene_->indexOfSlot[slot]);
    }
    // What changed after the frame it culled - placed, moved, shown - is
    // tested here, as an instance with no verdict always was.
    for (const uint32_t slot : changedSinceVerdict_) {
        if (slot >= slotCount || slotMark_[slot] == stamp) continue;
        if (slots.idAt(slot) == InstanceSlots::invalid) continue;
        slotMark_[slot] = stamp;
        out.push_back(scene_->indexOfSlot[slot]);
    }
    return false;
}

void M2Renderer::Frame::clear() noexcept {
    drawInstances_.clear(); sortedVisible_.clear(); transparentVisible_.clear();
    glowSprites_.clear(); ribbonRuns_.clear(); particleRuns_.clear();
    skeleton.clear(); particles.clear();
    cullUpdateSlots.clear(); cullUpdateRows.clear(); instanceUpdateRows.clear(); cullReset = false;
    materialUpdateSlots.clear(); materialUpdateRows.clear(); materialRowCount = 0;
    cullGuard = {}; culledFromColumns = false; cullSlotCount = cullSerial = 0;
    depth.clear(); solarDepth.clear(); strikeDepth.clear(); shadowModels.clear();
    instanceEntries = 0;
    skinnedDraws = 0;
    changesBy = {};
    solarMatrix = overheadMatrix = strikeMatrix = glm::mat4(0.0f);
}

M2Renderer::FrameCounts M2Renderer::recordedCounts() const {
    // Between recordings the renderer holds no frame (Renderer::endFrame
    // lets it go), and has nothing to count.
    if (!recording_) return {};
    const Frame& frame = *recording_;
    return {.cullCandidates = frame.cullSlotCount,
            .drawn = static_cast<uint32_t>(frame.drawInstances_.size()),
            .skinned = frame.skinnedDraws,
            .boneMatrices = frame.skeleton.boneCount,
            .sunCasters = static_cast<uint32_t>(frame.solarDepth.size()),
            .particleQuads = particlePass_.quadsLaidOut(),
            .instanceChanges = std::accumulate(frame.changesBy.begin(), frame.changesBy.end(), 0u),
            .culledFromColumns = frame.culledFromColumns,
            .changesBy = frame.changesBy};
}

std::shared_ptr<const M2Renderer::Frame> M2Renderer::extract(const Camera& camera, std::string_view stageGroup,
        const std::vector<LocalLight>& lights, float time, const glm::mat4& shadowMatrix, float shadowRadius,
        const glm::mat4* overheadMatrix, float overheadRadius,
        const glm::mat4* strikeMatrix, float strikeRadius, const PosedSkeletons* characters) {
    // The world's extraction was one number in the frame profile - 5.4 ms of
    // a 21.5 ms frame in Dalaran - that could not say which of these steps it
    // was. Each is timed apart.
    CpuStageLaps laps(vkCtx_ ? &vkCtx_->gameCpuStages() : nullptr, stageGroup);
    auto frame = framePool_.acquire();
    frame->changesBy = std::exchange(refiledBy_, {});
    frame->time = time;
    frame->cameraPosition = camera.getPosition();
    frame->skyHour = skyTimeOfDayHours_;
    frame->suppressStars = suppressBakedStars_;
    frame->skipDetail = skipGroundDetail_;
    frame->sceneIdentity = scene_->identity;
    frame->solarMatrix = shadowMatrix;
    frameLights_ = &lights;
    laps.lap("frame");
    if (initialized_) reapFinishedInstances();
    laps.lap("finished instances");
    extractCull(camera, *frame);
    laps.lap("view candidates");
    cullView(camera, *frame);
    laps.lap("visibility");
    // One base record and at most one override per batch (opaque UVs or
    // transparent draws). This bounds every pass before any command is recorded.
    for(const auto& draw:frame->drawInstances_)
        if(draw.model) frame->instanceEntries+=uint64_t(1)+draw.model->batches.size();
    gatherM2Particles(*frame, camera.getPosition(), characters);
    laps.lap("particles and ribbons");
    // The skeletons to pose: the drawn ones cullView asked for, those the
    // emitters run on, and the unseen ones near enough to shine - one
    // level, none rides another.
    frame->skeleton.view = camera.getViewMatrix();
    frame->skeleton.pages = skeletons_.pageBuffers();
    frame->skeleton.pagesVersion = skeletons_.version();
    poseUnseenLights(camera, *frame);
    frame->skeleton.levelEnds.push_back(static_cast<uint32_t>(frame->skeleton.requests.size()));
    laps.lap("skeletons");
    // The candidates that cast, as the shadow passes take them: all of it off
    // the cull record the cull just read and the slot's model, none of it off
    // the instance - where one stands is its slot's row on the GPU. Its model
    // is held by the frame once for each run of its casters - a shared_ptr
    // per caster was an atomic increment here and a decrement on the render
    // thread for each of some ten thousand casters a frame in Dalaran.
    const auto takeCasters = [&](std::vector<ShadowDraw>& into) {
        using G = M2CullGrid;
        constexpr uint16_t kNeeds = G::kValid | G::kHasModel;
        constexpr uint16_t kRefuses = G::kHidden | G::kFinishing | G::kInvisibleTrap;
        into.reserve(shadowCandidates_.size());
        uint32_t heldModelId = 0;
        const M2ModelGPU* heldModel = nullptr;
        for (uint32_t index : shadowCandidates_) {
            const M2CullGrid::Record& record = scene_->cullGrid_.record(index);
            if ((record.flags & kNeeds) != kNeeds || (record.flags & kRefuses)) continue;
            const uint32_t modelId = scene_->slots.modelAt(record.slot);
            if (!heldModel || modelId != heldModelId) {
                frame->shadowModels.push_back(models.at(modelId));
                heldModelId = modelId;
                heldModel = frame->shadowModels.back().get();
            }
            into.push_back({.modelId = modelId, .slot = record.slot, .model = heldModel,
                            .position = record.position, .reach = record.shadowRadius});
        }
    };
    if (shadowMatrix != glm::mat4(0.0f)) {
        gatherReusedShadowCandidates(shadowMatrix, shadowRadius);
        laps.lap("sun shadow candidates");
        takeCasters(frame->solarDepth);
    }
    laps.lap("sun shadow casters");
    if (overheadMatrix) {
        frame->overheadMatrix = *overheadMatrix;
        gatherShadowCandidates(*overheadMatrix, overheadRadius, shadowCandidates_);
        takeCasters(frame->depth);
    }
    if (strikeMatrix) {
        frame->strikeMatrix = *strikeMatrix;
        gatherShadowCandidates(*strikeMatrix, strikeRadius, shadowCandidates_);
        takeCasters(frame->strikeDepth);
    }
    laps.lap("overhead and lightning casters");
    frameLights_ = nullptr;
    return frame;
}


void M2Renderer::recordStarReplacement(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                       const M2ModelGPU& model, const StarReplacement& drawStars) {
    drawStars();
    // A different renderer just bound its pipeline, descriptors and vertices.
    // Restore all M2 bindings; callers invalidate their pipeline cache.
    const uint32_t frame = vkCtx_->getCurrentFrame();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
    const VkDescriptorSet bones = skeletonPass_.drawSet(frame);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &bones, 0, nullptr);
    if (instanceSet_[frame]) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelineLayout_, 3, 1, &instanceSet_[frame], 0, nullptr);
    bindTextureTable(cmd);
    geometry_.bind(cmd, model.geometry);
}

void M2Renderer::bindTextureTable(VkCommandBuffer cmd) const {
    const VkDescriptorSet table = vkCtx_->textureTable().set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &table, 0, nullptr);
}

void M2Renderer::recordView(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                            const StarReplacement& drawStars, bool drawBakedStars) {
    const auto& drawInstances_ = recording_->drawInstances_;

    lastDrawCallCount = 0;
    lastBlendedDrawCallCount = 0;
    skyDiagDrawsOpaque_ = 0;
    skyDiagDrawsTransparent_ = 0;
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if(recording_->instanceEntries>instanceCapacity_[frameIndex]) return;
    // Where they stand is in the instance table; without it (no memory,
    // logged by ensureSlotTables) nothing is drawn.
    if (!drawInstances_.empty() && !instanceSetReady(frameIndex)) return;

    // State tracking
    VkPipeline currentPipeline = VK_NULL_HANDLE;

    // Validate per-frame descriptor set before any Vulkan commands
    if (!perFrameSet) {
        LOG_ERROR("M2Renderer::render: perFrameSet is VK_NULL_HANDLE - skipping M2 render");
        return;
    }

    // Bind per-frame descriptor set (set 0) - shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
    bindTextureTable(cmd);

    // Start with opaque pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline_);
    currentPipeline = opaquePipeline_;
    // The sky model is seen from inside, so it shows every face.
    FaceCulling faces(*vkCtx_, cmd);

    // The frame's posed bones (set 2), which every skinned draw indexes from
    // its boneBase; bound for all, as a declared set must be.
    const VkDescriptorSet bones = skeletonPass_.drawSet(frameIndex);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &bones, 0, nullptr);

    // Bind the instance set (set 3): this frame's per-draw records, and the
    // instance table they name a slot of
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }

    // The records the passes write themselves start after the ones
    // prepareViewDraws placed; every pass of a frame writes the same ones.
    instanceDataCount_ = preparedRecordEnd_;

    // The opaque draws, written on the GPU (prepareViewDraws): a run of
    // commands from `first` on, its material rows read by gl_DrawID from
    // there.
    const auto drawCommands = [&](uint32_t first, uint32_t count) {
        while (count > 0) {
            const uint32_t n = std::min(count, maxDrawsPerCall_);
            const M2PushConstants push{.drawBase = first};
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            vkCmdDrawIndexedIndirect(cmd, drawCommands_[frameIndex].buffer,
                                     VkDeviceSize(first) * sizeof(VkDrawIndexedIndirectCommand), n,
                                     sizeof(VkDrawIndexedIndirectCommand));
            first += n;
            count -= n;
        }
    };
    const auto bindState = [&](uint8_t variant, bool twoSided) {
        const VkPipeline pipeline = variantPipeline(variant);
        if (pipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            faces.pipelineBound();
            currentPipeline = pipeline;
        }
        faces.show(twoSided);
    };
    if (viewDrawsReady_ && !skyMode_) {
        // A call a bucket: one pipeline, its faces, one geometry page.
        ::VkBuffer currentPage = VK_NULL_HANDLE;
        for (const ViewBucket& bucket : viewBuckets_) {
            bindState(bucket.variant, bucket.twoSided);
            if (bucket.page.vertices != currentPage) {
                geometry_.bind(cmd, bucket.page);
                currentPage = bucket.page.vertices;
            }
            drawCommands(bucket.first, bucket.count);
            lastDrawCallCount += bucket.count;
        }
    } else if (viewDrawsReady_) {
        // The sky, in its model's order: its layers blend over one another.
        // Its baked stars are left out where the client draws its own, which
        // then go where they were.
        std::optional<uint32_t> runFirst;
        uint32_t runCount = 0;
        std::optional<std::pair<uint8_t, bool>> runState;
        const M2ModelGPU* boundModel = nullptr;
        const auto flush = [&] {
            if (runFirst && runCount > 0) drawCommands(*runFirst, runCount);
            runFirst.reset();
            runCount = 0;
        };
        for (const SkyDraw& draw : skyDraws_) {
            const M2ModelGPU::DrawEntry& entry = draw.entry;
            if (entry.starLayer && recording_->suppressStars) {
                if (drawStars && entry.replacesStars) {
                    flush();
                    recordStarReplacement(cmd, perFrameSet, *draw.model, drawStars);
                    currentPipeline = VK_NULL_HANDLE;
                    runState.reset();
                    boundModel = draw.model;
                }
                continue;
            }
            if (entry.starLayer && !drawBakedStars) continue;
            const std::pair<uint8_t, bool> state{entry.variant, entry.twoSided};
            if (runState != state || draw.model != boundModel || !runFirst ||
                *runFirst + runCount != draw.command) {
                flush();
                bindState(entry.variant, entry.twoSided);
                runState = state;
                if (draw.model != boundModel) {
                    geometry_.bind(cmd, draw.model->geometry);
                    boundModel = draw.model;
                }
                runFirst = draw.command;
            }
            ++runCount;
            ++skyDiagDrawsOpaque_;
            ++lastDrawCallCount;
        }
        flush();
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
    if (!drawInstances_.empty() && !instanceSetReady(frameIndex)) return;
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // The sets the opaque pass bound, again: this may be a different command
    // buffer from the one it recorded into.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
    const VkDescriptorSet bones = skeletonPass_.drawSet(frameIndex);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &bones, 0, nullptr);
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }
    bindTextureTable(cmd);

    uint32_t currentModelId = UINT32_MAX;
    const M2ModelGPU* currentModel = nullptr;
    bool currentModelValid = false;
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    FaceCulling faces(*vkCtx_, cmd);
    auto runs = viewRuns(cmd, pipelineLayout_, drawCommands_[frameIndex], drawRecords_[frameIndex], frameDrawCursor(),
                         drawRoom(frameIndex), maxDrawsPerCall_);
    ::VkBuffer currentPage = VK_NULL_HANDLE;
    std::optional<bool> currentTwoSided;
    bool roomFull = false;

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
            if (!currentModel->geometry.placed()) continue;
            currentModelValid = true;
        }
        if (!currentModelValid) continue;

        const M2ModelGPU& model = *currentModel;

        // Authored/spawn opacity remains independent of distance culling.
        float instanceFadeAlpha = instance.fade;
        const bool instanceFaded = instance.fade < 0.999f;
        if (model.isGroundDetail) instanceFadeAlpha *= 0.82f;

        const bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
        const bool needsBones = modelNeedsAnimation && instance.boneCount > 0;
        if (needsBones && !posed_[frameIndex]) continue;

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
                    runs.flush();
                    recordStarReplacement(cmd, perFrameSet, model, drawStars);
                    currentPipeline = VK_NULL_HANDLE;
                    currentTwoSided.reset();
                    currentPage = model.geometry.vertices;
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
            e.slot = instance.slot;
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
            e.boneBase = needsBones ? static_cast<int32_t>(instance.boneBase) : 0;
            e.boneCount = static_cast<int32_t>(instance.boneCount);
            e.highlight = instance.highlight;
            e.flags = instance.afloat ? 1 : 0;
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
            // A faded opaque layer is blended and writes no depth: culled,
            // its far side no longer shows through its near one.
            const bool twoSided = skyMode_ || m2MaterialTwoSided(batch.materialFlags);

            if (batch.materialRow == kNoMaterialRow || roomFull) continue;

            // A new run where the state changes; within one the draws keep
            // their order back to front.
            if (desiredPipeline != currentPipeline || currentTwoSided != twoSided ||
                model.geometry.vertices != currentPage) {
                runs.flush();
                if (desiredPipeline != currentPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                    faces.pipelineBound();
                    currentPipeline = desiredPipeline;
                }
                faces.show(twoSided);
                currentTwoSided = twoSided;
                if (model.geometry.vertices != currentPage) {
                    geometry_.bind(cmd, model.geometry);
                    currentPage = model.geometry.vertices;
                }
            }
            if (!runs.add({.indexCount = batch.indexCount, .instanceCount = 1,
                           .firstIndex = model.geometry.firstIndex + batch.indexStart,
                           .vertexOffset = model.geometry.baseVertex, .firstInstance = drawOffset},
                          batch.materialRow)) {
                sayDrawRoomFull(drawRoom(frameIndex));
                roomFull = true;
                continue;
            }
            if (skyMode_) ++skyDiagDrawsTransparent_;
            lastDrawCallCount++;
            lastBlendedDrawCallCount++;
        }
    }
    runs.flush();

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
    static_assert(sizeof(M2ShadowInstance) == 16, "M2ShadowInstance is m2_shadow.vert's std430 layout");
    static_assert(sizeof(M2ShadowDraw) == 16, "M2ShadowDraw is m2_shadow.vert's uvec4");
    static_assert(sizeof(M2ShadowPush) <= 128, "the push block every device must take");

    // Binding 0: the frame's casters. Binding 1: the instance table they name
    // a slot of, written by ensureSlotTables. Binding 2: the frame's draws.
    VkDescriptorSetLayoutBinding bindings[3] = {};
    for (uint32_t b = 0; b < 3; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCi.bindingCount = 3;
    layoutCi.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &shadowInstanceSetLayout_) != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: could not create the shadow casters' set layout; doodads cast no shadow");
        return false;
    }

    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .descriptorCount = 3 * kShadowTexPoolFrames};
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
        // The draws' records and commands, mapped.
        const auto make = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, AllocatedBuffer& out) {
            VkBufferCreateInfo drawBci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            drawBci.size = bytes;
            drawBci.usage = usage;
            return gpuCreateBuffer(vkCtx_->getAllocator(), &drawBci, &aci, &out.buffer, &out.allocation,
                                   &out.info) == VK_SUCCESS &&
                   out.info.pMappedData;
        };
        if (!make(VkDeviceSize(kMaxShadowDraws) * sizeof(M2ShadowDraw), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  shadowDrawRecords_[i]) ||
            !make(VkDeviceSize(kMaxShadowDraws) * sizeof(VkDrawIndexedIndirectCommand),
                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, shadowDrawCommands_[i])) {
            LOG_ERROR("M2Renderer: could not allocate the shadow draws' buffers; doodads cast no shadow");
            return false;
        }

        VkDescriptorBufferInfo bufInfo{.buffer = shadowInstanceBuffer_[i], .offset = 0, .range = size};
        VkDescriptorBufferInfo drawInfo{.buffer = shadowDrawRecords_[i].buffer, .offset = 0, .range = VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t w = 0; w < 2; ++w) {
            writes[w] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[w].dstSet = shadowInstanceSet_[i];
            writes[w].dstBinding = w == 0 ? 0 : 2;
            writes[w].descriptorCount = 1;
            writes[w].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        writes[0].pBufferInfo = &bufInfo;
        writes[1].pBufferInfo = &drawInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
    return true;
}

bool M2Renderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    if (!createShadowInstanceBuffers()) return false;

    // Set 0: the texture table, which the draws' records name their
    // textures in. Set 1: the casters (M2ShadowInstance) and the draws.
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset = 0;
    pc.size = sizeof(M2ShadowPush);
    shadowPipelineLayout_ = createPipelineLayout(
        device, {vkCtx_->textureTable().layout(), shadowInstanceSetLayout_}, {pc});
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
    if (!fragShader.loadFromFile(device, "assets/shaders/m2_shadow.frag.spv")) {
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
    auto insideFootprint = [&](const ShadowDraw& caster) {
        if (perspective) return lightFrustum.intersectsSphere(caster.position, caster.reach);
        return instanceInShadowFootprint(lightSpaceMatrix, caster.position, caster.reach, shadowRadius);
    };


    (void)sunMap;
    (void)reuseCull;
    shadowDraws_.clear();
    shadowCasters_[0].clear();
    shadowCasters_[1].clear();
    const auto& candidates = lightSpaceMatrix == recording_->strikeMatrix ? recording_->strikeDepth
        : lightSpaceMatrix == recording_->overheadMatrix ? recording_->depth : recording_->solarDepth;
    for (const auto& source : candidates) {
        if (!insideFootprint(source)) continue;
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
        // What each draw is - the texture, the alpha test, the bend - is its
        // own record in this frame's draws, never a buffer the passes share:
        // a buffer is read when a draw executes, not when it is recorded, so
        // one written again for the foliage pass decided what the solid
        // pass's draws saw as well; a foliage batch that lost its alpha test
        // that way casts the whole leaf quad.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        const VkDescriptorSet table = vkCtx_->textureTable().set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            0, 1, &table, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            1, 1, &casterSet, 0, nullptr);
        // The draws, a call per run of one geometry page.
        const auto announce = [&](uint32_t first) {
            const M2ShadowPush push{.lightSpace = lightSpaceMatrix, .call = glm::uvec4(first, 0, 0, 0),
                                    .windPhases = wind.phases, .windDirLean = wind.dirLean};
            vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(M2ShadowPush),
                               &push);
        };
        DrawRuns<M2ShadowDraw, decltype(announce)> runs(
            cmd, shadowDrawCommands_[frameIdx].buffer, shadowDrawCommands_[frameIdx].info.pMappedData,
            shadowDrawRecords_[frameIdx].info.pMappedData, shadowDrawCount_, kMaxShadowDraws, maxDrawsPerCall_,
            announce);
        ::VkBuffer currentPage = VK_NULL_HANDLE;
        bool roomFull = false;

        const auto& casters = shadowCasters_[foliagePass ? 1 : 0];
        for (std::size_t g = 0; g < casters.size();) {
            const uint32_t modelId = casters[g].modelId;
            std::size_t groupEnd = g;
            while (groupEnd < casters.size() && casters[groupEnd].modelId == modelId) ++groupEnd;

            const M2ModelGPU* groupModel = shadowDraws_[casters[g].draw].model;
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
                const ShadowDraw& caster = shadowDraws_[casters[k].draw];
                // The height and amplitude are the model's, faded with the
                // caster's distance as the main pass fades it; the phase and
                // the wind's turn into its own space the shader takes from
                // its row, as m2.vert does.
                const float fade = windBent
                    ? m2WindDistanceFade(glm::length(caster.position - recording_->cameraPosition)) : 0.0f;
                casterData[firstCaster + (k - g)] = M2ShadowInstance{
                    .slot = caster.slot, .refHeight = sway.refHeight, .amplitude = sway.amp * fade};
            }

            for (const auto& batch : model.batches) {
                if (roomFull) break;
                // A layer that fades in and out is drawn blended, and blended
                // layers cast no shadow.
                if (batch.submeshLevel > 0 || batch.blendedByOpacity || !batch.shaderDrawn ||
                    !m2BatchCastsShadow(batch.blendMode, batch.batchOpacity,
                                        batch.glowCardLike, batch.starLayer)) continue;
                const bool alphaCutout = batch.blendMode == 1;
                // A leaf card with no texture is a shadow with no caster.
                //
                // Drawn without one, its alpha test would pass everywhere, so
                // a batch that arrived without its texture would cast the
                // shadow of a solid quad - while the main pass skips that same
                // batch. The result is a black rectangle on the ground under a
                // tree that looks fine, coming and going as the texture cache
                // lets the sheet back in.
                if (alphaCutout && (!batch.texture || batch.textureHandle == kNoTextureHandle)) {
                    if (!warnedShadowNoTexture_) {
                        warnedShadowNoTexture_ = true;
                        LOG_WARNING("Shadow pass: a foliage batch of ", model.name,
                                    " has no texture; skipping it rather than casting a "
                                    "solid quad. The texture cache is the usual reason.");
                    }
                    continue;
                }
                if (model.geometry.vertices != currentPage) {
                    runs.flush();
                    geometry_.bind(cmd, model.geometry);
                    currentPage = model.geometry.vertices;
                }
                // The texture through its own sampler, as the cache placed it.
                const M2ShadowDraw draw{
                    .texture = alphaCutout ? batch.textureHandle : kNoTextureHandle,
                    .flags = (alphaCutout ? 3u : 0u) | (windBent ? 4u : 0u),
                    .alphaRef = batch.drawState.alphaTest == 4 ? 224u : 0u};
                if (!runs.add({.indexCount = batch.indexCount, .instanceCount = count,
                               .firstIndex = model.geometry.firstIndex + batch.indexStart,
                               .vertexOffset = model.geometry.baseVertex, .firstInstance = firstCaster},
                              draw)) {
                    static core::LogBudget budget(4, "M2 shadow draw room full");
                    if (budget.take())
                        LOG_WARNING("Shadow pass: this frame's maps filled their ", kMaxShadowDraws,
                                    " draws - the doodads after them cast no shadow this frame");
                    roomFull = true;
                }
            }
            castersDrawn += count;
            g = groupEnd;
        }
        runs.flush();
    }
}

void M2Renderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, const WindGpu& wind,
                              const glm::vec3& /*shadowCenter*/, float shadowRadius, bool resetDescriptors,
                              bool reuseCull) {
    if (!shadowPipeline_) return;
    if (recording_->depth.empty() && recording_->solarDepth.empty() && recording_->strikeDepth.empty()) return;
    const uint32_t frameIdx = vkCtx_->getCurrentFrame();

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
    // The sun's map is a frame's first: the casters and draws start again.
    if (resetDescriptors) {
        shadowInstanceCount_ = 0;
        shadowDrawCount_ = 0;
    }
    // Where the casters stand is in the instance table; without it (no
    // memory, logged by ensureSlotTables) the doodads cast no shadow.
    if (!shadowInstanceMapped_[frameIdx] || !shadowInstanceSet_[frameIdx] || !shadowSetReady(frameIdx)) return;
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
