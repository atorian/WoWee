#include <cstdlib>
#include "pipeline/heap_bytes.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/m2_renderer_internal.h"
#include "pipeline/m2_model_classifier.hpp"
#include "rendering/texture_loads.hpp"
#include "rendering/texture_upload.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/shadow_cull_reuse.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <set>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace wowee {
namespace rendering {

void M2Renderer::placeInCollision(const M2Instance& instance) {
    if (!collision_ || !instance.cachedModel || !instance.cachedModel->collision) return;
    collision_->placeM2(instance.id, instance.cachedModel->collision, instance.modelMatrix,
                        instance.scale);
    if (instance.hidden) collision_->setM2Hidden(instance.id, true);
}

void M2Renderer::fileInCullGrid(size_t index, InstanceChange why) {
    ++refiledBy_[static_cast<size_t>(why)];
    const M2Instance& instance = scene_->instances[index];
    scene_->slots.changed(instance.id);
    auto& record = scene_->cullGrid_.record(static_cast<uint32_t>(index));
    record.position = instance.position;
    record.cullCenter = instance.cachedCullCenter;
    record.paddedRadius = instance.cachedPaddedRadius;
    record.maxDistSqFactor = instance.cachedEffectiveMaxDistSqFactor;
    record.id = instance.id;
    record.slot = scene_->slots.slotOf(instance.id);
    // The margin the shadow pass inflates a position by, before its division
    // by the pass's radius: the model's bound radius at this scale, times 1.5.
    const float boundRadius = instance.cachedModel ? instance.cachedModel->boundRadius
                                                   : instance.cachedBoundRadius;
    record.shadowRadius = boundRadius * instance.scale * 1.5f;
    using G = M2CullGrid;
    record.flags = static_cast<uint16_t>(
        (instance.cachedIsValid ? G::kValid : 0) | (instance.cachedModel ? G::kHasModel : 0) |
        (instance.cachedIsInvisibleTrap ? G::kInvisibleTrap : 0) |
        (instance.cachedIsGroundDetail ? G::kGroundDetail : 0) |
        (instance.cachedIsSkyBird && instance.cachedHasAnimation && !instance.cachedDisableAnimation
             ? G::kAnimatedSkyBird : 0) |
        (instance.isGameObject ? G::kGameObject : 0) | (instance.hidden ? G::kHidden : 0) |
        (instance.finishing ? G::kFinishing : 0));
    // A hidden instance is in no column, however it moves meanwhile.
    scene_->cullGrid_.file(static_cast<uint32_t>(index));
    if (instance.hidden) return;
    // The sun's shadow cull reuses its candidates across frames; this one
    // arrived or moved since, and is added to them (gatherReusedShadowCandidates).
    // Past this many it is cheaper to cull again than to fold them in.
    constexpr size_t kMaxShadowRefiled = 8192;
    if (scene_->shadowRefiledIds.size() < kMaxShadowRefiled) {
        scene_->shadowRefiledIds.push_back(instance.id);
    } else {
        scene_->shadowRefiledOverflow = true;
    }
}

void M2Renderer::gatherViewCandidates(const glm::vec3& camPos, const Frustum& frustum,
                                      float maxPossibleDistSq, std::vector<uint32_t>& out) const {
    out.clear();
    for (const auto& cell : scene_->cullGrid_.cells()) {
        if (cell.members.empty()) continue;
        // Nothing in a column is drawn further away than this, and nothing
        // whose cull sphere is outside the frustum - and the column's box
        // holds every position and every sphere filed in it.
        if (distanceSqToBox(camPos, cell.viewMin, cell.viewMax) > maxPossibleDistSq) continue;
        if (!frustum.intersectsAABB(cell.viewMin, cell.viewMax)) continue;
        out.insert(out.end(), cell.members.begin(), cell.members.end());
    }
}

void M2Renderer::gatherShadowCandidates(const glm::mat4& lightSpace, float shadowRadius,
                                        std::vector<uint32_t>& out) const {
    out.clear();
    const float invRadius = shadowRadius > 0.0f ? 1.0f / shadowRadius : 0.0f;
    const bool perspective = glm::length(glm::vec3(lightSpace[0][3], lightSpace[1][3], lightSpace[2][3])) > 1e-6f;
    Frustum frustum;
    if (perspective) frustum.extractFromMatrix(lightSpace);
    for (const auto& cell : scene_->cullGrid_.cells()) {
        if (cell.members.empty()) continue;
        // The light's box, not the camera's: a caster behind the camera can
        // still throw its shadow into view.
        if (perspective) {
            const glm::vec3 margin(cell.maxShadowRadius);
            if (!frustum.intersectsAABB(cell.posMin - margin, cell.posMax + margin)) continue;
        } else if (!cellMayCastShadow(lightSpace, cell.posMin, cell.posMax,
                                      cell.maxShadowRadius * invRadius)) continue;
        out.insert(out.end(), cell.members.begin(), cell.members.end());
    }
}

void M2Renderer::gatherReusedShadowCandidates(const glm::mat4& lightSpace, float shadowRadius) {
    // How much further than its own margin a candidate may lie, in the map's
    // clip units: a twentieth of the map, 25 yards at the default 500. The
    // texel snap and the sun's turn take it up a little a frame.
    constexpr float kShadowCullSlack = 0.05f;
    const float invRadius = shadowRadius > 0.0f ? 1.0f / shadowRadius : 0.0f;
    // Read from the instances' cull records (M2CullGrid::Record).
    const auto marginOf = [&](const M2CullGrid::Record& r) { return r.shadowRadius * invRadius; };
    // Against the map the candidates were culled for: a refiled instance
    // joins them by the same test the others passed.
    const auto withinSlackOf = [&](const glm::mat4& map, const M2CullGrid::Record& r) {
        const glm::vec4 clip = map * glm::vec4(r.position, 1.0f);
        const float m = marginOf(r) + kShadowCullSlack;
        return std::abs(clip.x) <= 1.0f + m && std::abs(clip.y) <= 1.0f + m &&
               clip.z >= -m && clip.z <= 1.0f + m;
    };

    // What can cast at all, and stays so for the instance's life: a valid
    // model that is not a trap. Whether it is hidden or on its way out the
    // caller's exact test asks every frame.
    const auto canCast = [](const M2CullGrid::Record& r) {
        constexpr uint16_t kNeeds = M2CullGrid::kValid | M2CullGrid::kHasModel;
        return (r.flags & kNeeds) == kNeeds && !(r.flags & M2CullGrid::kInvisibleTrap);
    };
    const auto& grid = scene_->cullGrid_;
    // Kept in the order the pass draws them in - by model, then index - so
    // the casters it picks out come out grouped without sorting them anew.
    const auto drawOrder = [this](uint32_t a, uint32_t b) {
        const uint32_t modelA = scene_->instances[a].modelId;
        const uint32_t modelB = scene_->instances[b].modelId;
        return modelA != modelB ? modelA < modelB : a < b;
    };

    bool reuse = shadowCullValid_ && shadowCullScene_ == scene_ && shadowCullRadius_ == shadowRadius &&
                 shadowCullLayoutRevision_ == instanceLayoutRevision_ && !scene_->shadowRefiledOverflow;
    if (reuse) {
        // What arrived or moved since may carry a larger margin than anything
        // the last cull saw; the region the check tests grows with it.
        for (const uint32_t id : scene_->shadowRefiledIds) {
            const auto it = scene_->instanceIndexById.find(id);
            if (it == scene_->instanceIndexById.end() || it->second >= scene_->instances.size()) continue;
            shadowCullMaxMargin_ = std::max(shadowCullMaxMargin_,
                                            marginOf(grid.record(static_cast<uint32_t>(it->second))));
        }
        reuse = lightSpaceCullStillCovers(shadowCullMatrix_, lightSpace, glm::vec3(-1.0f, -1.0f, 0.0f),
                                          glm::vec3(1.0f), shadowCullMaxMargin_, kShadowCullSlack);
    }
    if (reuse) {
        // The refiled ones join, once each, where the draw order puts them;
        // one that has left fails the caller's exact test.
        for (const uint32_t id : scene_->shadowRefiledIds) {
            const auto it = scene_->instanceIndexById.find(id);
            if (it == scene_->instanceIndexById.end() || it->second >= scene_->instances.size()) continue;
            const auto index = static_cast<uint32_t>(it->second);
            const auto& record = grid.record(index);
            if (!canCast(record) || !withinSlackOf(shadowCullMatrix_, record)) continue;
            const auto at = std::lower_bound(shadowCullIndices_.begin(), shadowCullIndices_.end(), index,
                                             drawOrder);
            if (at == shadowCullIndices_.end() || *at != index) shadowCullIndices_.insert(at, index);
        }
    } else {
        // Every column, for the largest margin anywhere; the ones that may
        // cast with the slack, for their instances.
        shadowCullIndices_.clear();
        shadowCullMaxMargin_ = 0.0f;
        for (const auto& cell : grid.cells()) {
            if (cell.members.empty()) continue;
            shadowCullMaxMargin_ = std::max(shadowCullMaxMargin_, cell.maxShadowRadius * invRadius);
            if (!cellMayCastShadow(lightSpace, cell.posMin, cell.posMax,
                                   cell.maxShadowRadius * invRadius + kShadowCullSlack)) continue;
            for (const uint32_t index : cell.members) {
                const auto& record = grid.record(index);
                if (canCast(record) && withinSlackOf(lightSpace, record)) shadowCullIndices_.push_back(index);
            }
        }
        std::sort(shadowCullIndices_.begin(), shadowCullIndices_.end(), drawOrder);
        shadowCullMatrix_ = lightSpace;
        shadowCullRadius_ = shadowRadius;
        shadowCullScene_ = scene_;
        shadowCullLayoutRevision_ = instanceLayoutRevision_;
        shadowCullValid_ = true;
    }
    scene_->shadowRefiledIds.clear();
    scene_->shadowRefiledOverflow = false;
    shadowCandidates_ = shadowCullIndices_;
}

void M2Renderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return;
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    // Where it already stands: nothing changes (see setInstanceTransform).
    if (inst.position == position) return;

    inst.position = position;
    inst.updateModelMatrix();
    // Use cachedModel instead of a fresh models.find() - the pointer was set
    // at addInstance and stays valid as long as the instance exists.
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        collisionLocalBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }
    fileInCullGrid(idxIt->second, InstanceChange::Moved);
    if (collision_) collision_->moveM2(instanceId, inst.modelMatrix, inst.scale);
}

void M2Renderer::setInstanceFade(uint32_t instanceId, float alpha) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    const float next = std::clamp(alpha, 0.0f, 1.0f);
    // A faded instance leaves the ray tracer's copy, which reads it again.
    if ((inst.fade >= 1.0f) != (next >= 1.0f)) scene_->slots.changed(instanceId);
    inst.fade = next;
}

void M2Renderer::setInstanceHidden(uint32_t instanceId, bool hidden) {
    setInstancesHidden({instanceId}, hidden);
}

void M2Renderer::setInstancesHidden(const std::vector<uint32_t>& instanceIds, bool hidden) {
    // Out of the world is out of every index the passes walk - the cull grid
    // the view, shadow and GPU cull passes gather from, and the animated,
    // particle and light lists - and out of the collision world: no floor or
    // wall. The few walks over every instance check the flag itself.
    for (const uint32_t id : instanceIds) {
        auto idxIt = scene_->instanceIndexById.find(id);
        if (idxIt == scene_->instanceIndexById.end()) continue;
        auto& inst = scene_->instances[idxIt->second];
        if (inst.hidden == hidden) continue;
        inst.hidden = hidden;
        if (collision_) collision_->setM2Hidden(id, hidden);
        if (hidden) {
            // Its clocks stop while it is away and resume where they were.
            inst.hiddenAtMs = animClockMs_;
            dropFromIndexSets(idxIt->second);
            fileInCullGrid(idxIt->second, InstanceChange::ShownOrHidden);
            // What it had emitted goes with it rather than hanging where it
            // was, or reappearing there, stale, when it comes back.
            releaseParticles(inst);
        } else {
            const double away = animClockMs_ - inst.hiddenAtMs;
            inst.animStartMs += away;
            inst.globalSequenceStartMs += away;
            fileInCullGrid(idxIt->second, InstanceChange::ShownOrHidden);
            // Back as if newly placed: no cull verdict to trust.
            auto& record = scene_->cullGrid_.record(static_cast<uint32_t>(idxIt->second));
            record.lastVisible = 1;
            fileInIndexSets(idxIt->second);
        }
    }
}

void M2Renderer::setInstanceHighlight(uint32_t instanceId, float amount) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    scene_->instances[idxIt->second].highlight = amount;
}

void M2Renderer::clearInstanceHighlights() {
    // A walk of every instance rather than a record of the lit one, because a
    // press can end with the instance gone - the object despawned, the tile
    // unloaded - and a remembered id would then light whatever took its slot.
    for (auto& inst : scene_->instances) inst.highlight = 0.0f;
}

void M2Renderer::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    inst.animSpeed = frozen ? 0.0f : 1.0f;
    inst.holdAtEnd = false;
    inst.nextAnimation.reset();
    if (frozen) {
        setAnimTime(inst, 0.0f);  // Reset to bind pose
    }
}

void M2Renderer::restartInstanceAnimation(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    setAnimTime(inst, 0.0f);
}

void M2Renderer::setInstanceAnimationHeld(uint32_t instanceId, uint32_t animationId,
                                          bool skipToEnd) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id != animationId) continue;
        inst.currentSequenceIndex = i;
        inst.animDuration = static_cast<float>(seqs[i].duration);
        inst.playingVariation = false;   // not a variation: it does not go back
        inst.holdAtEnd = true;
        inst.nextAnimation.reset();
        if (skipToEnd) {
            setAnimTime(inst, inst.animDuration);
            inst.animSpeed = 0.0f;
        } else {
            setAnimTime(inst, 0.0f);
            inst.animSpeed = 1.0f;
        }
        return;
    }
}

namespace {

// Put one planned animation on an instance. False when the model lacks it.
// The sequence time it starts at goes to `startTime`, for the caller to set.
bool startPlannedAnimation(M2Instance& inst, const M2AnimationPlay& play, float& startTime) {
    const auto& seqs = inst.cachedModel->sequences;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id != play.animationId) continue;
        inst.currentSequenceIndex = i;
        inst.animDuration = static_cast<float>(seqs[i].duration);
        inst.playingVariation = false;
        // A sequence flagged 0x01 plays once and stops at its end even when
        // it is the resting one (WoW.exe 0x832260).
        constexpr uint32_t kM2SequencePlaysOnce = 0x01;
        inst.holdAtEnd = play.end != M2AnimationPlay::End::Loop ||
                         (seqs[i].flags & kM2SequencePlaysOnce) != 0;
        const float start = play.reversed ? inst.animDuration : 0.0f;
        const float end = play.reversed ? 0.0f : inst.animDuration;
        if (play.frozen || play.startAtEnd) {
            startTime = play.frozen ? start : end;
            inst.animSpeed = 0.0f;
            inst.holdAtEnd = true;
        } else {
            startTime = start;
            inst.animSpeed = play.reversed ? -1.0f : 1.0f;
        }
        return true;
    }
    return false;
}

} // namespace

bool M2Renderer::playInstanceAnimation(uint32_t instanceId, const M2AnimationPlay& now,
                                       const std::optional<M2AnimationPlay>& next) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return false;
    auto& inst = scene_->instances[idxIt->second];
    if (!inst.cachedModel) return false;
    float startTime = 0.0f;
    if (!startPlannedAnimation(inst, now, startTime)) return false;
    setAnimTime(inst, startTime);
    inst.nextAnimation = now.end == M2AnimationPlay::End::Next ? next : std::nullopt;
    return true;
}

void M2Renderer::advanceToNextAnimation(M2Instance& inst) {
    const M2AnimationPlay next = *inst.nextAnimation;
    inst.nextAnimation.reset();
    float startTime = 0.0f;
    if (startPlannedAnimation(inst, next, startTime)) {
        setAnimTime(inst, startTime);
    } else {
        LOG_WARNING("M2: '", inst.cachedModel->name, "' has no animation ", next.animationId,
                    " to go on to; holding where it is");
        inst.animSpeed = 0.0f;
        inst.holdAtEnd = true;
    }
}

void M2Renderer::setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    // Find the first sequence matching the requested animation ID
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id == animationId) {
            inst.currentSequenceIndex = i;
            inst.animDuration = static_cast<float>(seqs[i].duration);
            setAnimTime(inst, 0.0f);
            inst.animSpeed = 1.0f;
            // Use playingVariation=true for one-shot (returns to idle when done)
            inst.playingVariation = !loop;
            inst.holdAtEnd = false;
            inst.nextAnimation.reset();
            return;
        }
    }
}

bool M2Renderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return false;
    const auto& inst = scene_->instances[idxIt->second];
    if (!inst.cachedModel) return false;
    for (const auto& seq : inst.cachedModel->sequences) {
        if (seq.id == animationId) return true;
    }
    return false;
}

bool M2Renderer::modelHasAnimation(uint32_t modelId, uint32_t animationId) const {
    const auto it = models.find(modelId);
    if (it == models.end()) return false;
    for (const auto& seq : it->second->sequences) {
        if (seq.id == animationId) return true;
    }
    return false;
}

float M2Renderer::modelAnimationDuration(uint32_t modelId, uint32_t animationId) const {
    const auto it = models.find(modelId);
    if (it == models.end()) return 0.0f;
    for (const auto& seq : it->second->sequences) {
        if (seq.id == animationId) return static_cast<float>(seq.duration);
    }
    return 0.0f;
}

uint32_t M2Renderer::getInstanceModelId(uint32_t instanceId) const {
    const auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return 0;
    return scene_->instances[idxIt->second].modelId;
}

bool M2Renderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return false;
    const auto& inst = scene_->instances[idxIt->second];
    outCenter = inst.cachedCullCenter;
    outRadius = inst.cachedVisualRadius;
    return outRadius > 0.0f;
}

void M2Renderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    // Reject NaN matrix - would propagate into the model matrix uniform
    // and the grids' bounds, leaving stale cells pointing at a NaN-bounded
    // instance.
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(transform[c][r])) return;
    auto& inst = scene_->instances[idxIt->second];
    // Where it already stands. A transport waiting at its dock is set again
    // every frame and carries its doodads with it; a spell effect on a unit
    // that stands still is set every frame too. Neither moves anything.
    if (inst.modelMatrix == transform) return;
    const glm::vec3 oldPosition = inst.position;

    // Update model matrix directly
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    // Extract position from transform for bounds
    inst.position = glm::vec3(transform[3]);

    // The dedup map is keyed on position, so it has to move with the instance.
    // It did not, and only a rebuild of the whole index ever put it right -
    // which this path deliberately avoids. A ship's doodads are created at the origin and
    // moved here a frame later, so the origin key stayed pointing at them and
    // the next ship of the same class was handed the first ship's sails instead
    // of its own. Both hulls then wrote their transform to the one instance, so
    // it rendered at whichever ship updated last, and whichever hull unloaded
    // first destroyed it for the other.
    if (!inst.cachedIsGroundDetail) {
        auto keyFor = [&](const glm::vec3& p) {
            return DedupKey{.modelId = inst.modelId,
                            .qx = static_cast<int32_t>(std::round(p.x * 10.0f)),
                            .qy = static_cast<int32_t>(std::round(p.y * 10.0f)),
                            .qz = static_cast<int32_t>(std::round(p.z * 10.0f))};
        };
        const DedupKey oldKey = keyFor(oldPosition);
        const DedupKey newKey = keyFor(inst.position);
        if (!(oldKey == newKey)) {
            auto oldIt = scene_->instanceDedupMap_.find(oldKey);
            // Only drop the entry if it still names this instance.
            if (oldIt != scene_->instanceDedupMap_.end() && oldIt->second == instanceId) {
                scene_->instanceDedupMap_.erase(oldIt);
            }
            scene_->instanceDedupMap_.emplace(newKey, instanceId);
        }
    }

    // Update bounds via the cached model pointer
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        collisionLocalBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }
    fileInCullGrid(idxIt->second, InstanceChange::Moved);
    if (collision_) collision_->moveM2(instanceId, inst.modelMatrix, inst.scale);
}

void M2Renderer::removeInstance(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    size_t idx = idxIt->second;
    if (idx >= scene_->instances.size()) return;
    ++instanceLayoutRevision_;  // the last instance moves into its place

    auto& inst = scene_->instances[idx];

    scene_->rtChanges.release(scene_->slots.slotOf(instanceId), rtScene_);
    scene_->slots.remove(instanceId);
    if (collision_) collision_->removeM2(instanceId);

    // Remove from dedup map
    if (!inst.cachedIsGroundDetail) {
        DedupKey dk{.modelId = inst.modelId,
                    .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
        // Only if it names this instance: one with an id of its own was never
        // filed, and another may stand at the same spot.
        auto dedupIt = scene_->instanceDedupMap_.find(dk);
        if (dedupIt != scene_->instanceDedupMap_.end() && dedupIt->second == instanceId) {
            scene_->instanceDedupMap_.erase(dedupIt);
        }
    }
    releaseParticles(inst);

    // Swap-remove: move last element to the hole and pop_back to avoid O(n) shift
    scene_->instanceIndexById.erase(instanceId);
    scene_->cullGrid_.removeSwapLast(static_cast<uint32_t>(idx));
    const size_t last = scene_->instances.size() - 1;
    if (idx < last) {
        uint32_t movedId = scene_->instances.back().id;
        scene_->instances[idx] = std::move(scene_->instances.back());
        scene_->instances.pop_back();
        scene_->instanceIndexById[movedId] = idx;
    } else {
        scene_->instances.pop_back();
    }

    // The index sets follow the one move rather than being rebuilt over
    // every instance, which made a despawn of a few dozen objects cost tens
    // of milliseconds.
    dropFromIndexSets(idx);
    if (idx < last) moveInIndexSets(last, idx);
}

void M2Renderer::fileInIndexSets(size_t index) {
    const M2Instance& ri = scene_->instances[index];
    // Not animated, emitting or lighting anything while out of the world.
    if (ri.hidden) return;
    if (ri.cachedHasEmitters) scene_->particleInstanceIndices_.insert(index);
    if (ri.cachedHasAnimation && !ri.cachedDisableAnimation)
        scene_->animatedInstanceIndices_.insert(index);
    else if (ri.cachedHasEmitters)
        scene_->particleOnlyInstanceIndices_.insert(index);
    if (ri.cachedModel && ri.cachedModel->canThrowLocalLight())
        scene_->lightInstanceIndices_.insert(index);
}

void M2Renderer::dropFromIndexSets(size_t index) {
    scene_->animatedInstanceIndices_.erase(index);
    scene_->particleOnlyInstanceIndices_.erase(index);
    scene_->particleInstanceIndices_.erase(index);
    scene_->lightInstanceIndices_.erase(index);
}

void M2Renderer::moveInIndexSets(size_t from, size_t to) {
    const uint32_t slot = scene_->slots.slotOf(scene_->instances[to].id);
    if (slot < scene_->indexOfSlot.size()) scene_->indexOfSlot[slot] = static_cast<uint32_t>(to);
    scene_->animatedInstanceIndices_.move(from, to);
    scene_->particleOnlyInstanceIndices_.move(from, to);
    scene_->particleInstanceIndices_.move(from, to);
    scene_->lightInstanceIndices_.move(from, to);
}

void M2Renderer::setInstanceIsGameObject(uint32_t instanceId, bool isGameObject) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return;
    scene_->instances[idxIt->second].isGameObject = isGameObject;
    fileInCullGrid(idxIt->second, InstanceChange::Other);
}

void M2Renderer::setSkipCollision(uint32_t instanceId, bool skip) {
    if (collision_) collision_->setM2SkipCollision(instanceId, skip);
}

void M2Renderer::setSkipWallCollision(uint32_t instanceId, bool skip) {
    if (collision_) collision_->setM2SkipWalls(instanceId, skip);
}

void M2Renderer::setInteriorLight(uint32_t instanceId, const std::optional<InteriorLight>& light) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return;
    scene_->instances[idxIt->second].interiorLight = packInteriorLight(light);
    // The GPU's instance table holds it.
    scene_->slots.changed(instanceId);
}

void M2Renderer::finishInstance(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    if (inst.finishing) return;
    inst.finishing = true;
    inst.particleCountsAtFinish = inst.particleCounts;
    fileInCullGrid(idxIt->second, InstanceChange::Finished);
    // Its particles have at most their longest life left to run.
    float longestLife = 0.0f;
    if (inst.cachedModel) {
        for (const auto& em : inst.cachedModel->particleEmitters) {
            longestLife = std::max(longestLife,
                interpFloat(em.lifespan, animTimeOf(inst), globalSequenceTimeOf(inst),
                            inst.currentSequenceIndex, inst.cachedModel->globalSequenceDurations));
        }
    }
    finishingInstances_.push_back({.instanceId = instanceId,
                                   .deadline = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(static_cast<int64_t>(longestLife * 1000.0f) + 100)});
}

void M2Renderer::reapFinishedInstances() {
    if (finishingInstances_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> done;
    std::erase_if(finishingInstances_, [&](const FinishingInstance& f) {
        auto idxIt = scene_->instanceIndexById.find(f.instanceId);
        if (idxIt == scene_->instanceIndexById.end()) return true;  // removed meanwhile
        const auto& inst = scene_->instances[idxIt->second];
        // Its particles and edges are counted on the GPU, two frames late:
        // until a count from a frame that ran it finished has come back - the
        // third after - what is known is from before. One with no room has
        // none.
        const auto placed = [](const std::vector<ParticlePool::Range>& ranges) {
            return std::any_of(ranges.begin(), ranges.end(), [](const ParticlePool::Range& r) { return r.placed(); });
        };
        const bool emittedLeft = (placed(inst.particleRanges) || placed(inst.ribbonRanges)) &&
                                 (inst.particleCounts - inst.particleCountsAtFinish < 3 ||
                                  inst.liveParticles > 0 || inst.liveEdges > 0);
        // Gone when nothing it emitted is left - or when its particles have
        // had their whole life, though it was out of sight and not updated.
        if (!emittedLeft || now >= f.deadline) {
            done.push_back(f.instanceId);
            return true;
        }
        return false;
    });
    if (!done.empty()) removeInstances(done);
}

void M2Renderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (instanceIds.empty() || scene_->instances.empty()) {
        return;
    }
    ++instanceLayoutRevision_;

    // A tile's doodads leave together, and this used to compact the array and
    // then rebuild the whole index - the id map and the dedup map, for every
    // instance still in the world. That was the 70-130 ms hitch each tile
    // unload cost while moving. Only what the leaving instances own is
    // touched now: their dedup keys, their placements and the slots they
    // vacate.
    std::unordered_set<uint32_t> toRemove;
    toRemove.reserve(instanceIds.size());
    std::vector<size_t> holes;
    holes.reserve(instanceIds.size());
    for (uint32_t id : instanceIds) {
        auto idxIt = scene_->instanceIndexById.find(id);
        if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) continue;
        if (!toRemove.insert(id).second) continue;
        auto& inst = scene_->instances[idxIt->second];
        holes.push_back(idxIt->second);
        scene_->rtChanges.release(scene_->slots.slotOf(id), rtScene_);
        scene_->slots.remove(id);
        if (collision_) collision_->removeM2(id);

        if (!inst.cachedIsGroundDetail) {
            DedupKey dk{.modelId = inst.modelId,
                        .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                        .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                        .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
            // Only drop the entry if it still names this instance.
            auto dedupIt = scene_->instanceDedupMap_.find(dk);
            if (dedupIt != scene_->instanceDedupMap_.end() && dedupIt->second == id) {
                scene_->instanceDedupMap_.erase(dedupIt);
            }
        }
        releaseParticles(inst);
        scene_->instanceIndexById.erase(idxIt);
    }
    if (holes.empty()) return;

    // Fill the holes from the back, highest first, so a slot being filled is
    // never one still waiting to be emptied: everything past the highest
    // remaining hole is a survivor.
    std::sort(holes.begin(), holes.end(), std::greater<>());
    for (size_t hole : holes) {
        const size_t last = scene_->instances.size() - 1;
        dropFromIndexSets(hole);
        scene_->cullGrid_.removeSwapLast(static_cast<uint32_t>(hole));
        if (hole != last) {
            scene_->instances[hole] = std::move(scene_->instances[last]);
            scene_->instanceIndexById[scene_->instances[hole].id] = hole;
            moveInIndexSets(last, hole);
        }
        scene_->instances.pop_back();
    }
}

void M2Renderer::clear() {
    scene_->rtChanges.clear(rtScene_);
    scene_->slots.clear();
    scene_->indexOfSlot.clear();
    scene_->identity = nextSceneIdentity_.fetch_add(1, std::memory_order_relaxed);
    ++instanceLayoutRevision_;
    if (vkCtx_) {
        // Their owners see them gone (hasPosedEmitters) and ask again.
        destroyPosedEmitterModels();
        for (auto& [id, modelResource] : models) {
            destroyModelGPU(id, modelResource);
        }
        // Each retired model returns its own sets after the fences.
    }
    models.clear();
    pinnedModelIds_.clear();
    for (auto& inst : scene_->instances) releaseParticles(inst);
    scene_->instances.clear();
    if (collision_) collision_->clearM2();
    scene_->instanceIndexById.clear();
    scene_->cullGrid_.clear();
    scene_->instanceDedupMap_.clear();
    scene_->animatedInstanceIndices_.clear();
    scene_->particleOnlyInstanceIndices_.clear();
    scene_->particleInstanceIndices_.clear();
    scene_->lightInstanceIndices_.clear();

    // The models' textures went back to the shared cache with the models,
    // where nothing references them and eviction makes room from them first.
    loggedTextureLoadFails_.clear();
}

void M2Renderer::clearInstances() {
    scene_->rtChanges.clear(rtScene_);
    scene_->slots.clear();
    scene_->indexOfSlot.clear();
    ++instanceLayoutRevision_;
    for (auto& inst : scene_->instances) releaseParticles(inst);
    for (auto& [id, posed] : scene_->posedEmitters_) releaseParticles(posed.inst);
    *scene_ = InstanceState{};
    if (collision_) collision_->clearM2();
}

void M2Renderer::setModelPinned(uint32_t modelId, bool pinned) {
    if (pinned) {
        pinnedModelIds_.insert(modelId);
        modelUnusedSince_.erase(modelId);
    } else {
        pinnedModelIds_.erase(modelId);
    }
}

void M2Renderer::cleanupUnusedModels() {
    // Build set of model IDs that are still referenced by instances
    std::unordered_set<uint32_t> usedModelIds;
    for (const auto& instance : scene_->instances) {
        usedModelIds.insert(instance.modelId);
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kGracePeriod = std::chrono::seconds(60);

    // Find models with no instances that have exceeded the grace period.
    // Models that just lost their last instance get tracked but not evicted
    // immediately - this prevents thrashing when GO models are briefly
    // instance-free between despawn and respawn cycles.
    std::vector<uint32_t> toRemove;
    for (const auto& [id, modelResource] : models) {
        (void)modelResource;
        if (usedModelIds.find(id) != usedModelIds.end() ||
            pinnedModelIds_.find(id) != pinnedModelIds_.end()) {
            // Model still in use or pinned - clear any pending unused timestamp
            modelUnusedSince_.erase(id);
            continue;
        }
        auto unusedIt = modelUnusedSince_.find(id);
        if (unusedIt == modelUnusedSince_.end()) {
            // First cycle with no instances - start the grace timer
            modelUnusedSince_[id] = now;
        } else if (now - unusedIt->second >= kGracePeriod) {
            // Grace period expired - mark for removal
            toRemove.push_back(id);
            modelUnusedSince_.erase(unusedIt);
        }
    }

    // Retire through the same fence queue as explicit unload/map transitions.
    for (uint32_t id : toRemove) {
        auto it = models.find(id);
        if (it != models.end()) {
            destroyModelGPU(id, it->second);
            models.erase(it);
            // Record the eviction so owners caching "uploaded" model IDs can
            // drop it; otherwise their next spawn skips the load as a stale hit
            // and pushes an empty placeholder (missing doodads on revisit).
            reapedModelIds_.push_back(id);
        }
    }

    if (!toRemove.empty()) {
        LOG_INFO("M2 cleanup: removed ", toRemove.size(), " unused models, ", models.size(), " remaining");
    }
}

std::vector<uint32_t> M2Renderer::drainReapedModelIds() {
    std::vector<uint32_t> out;
    out.swap(reapedModelIds_);
    return out;
}

void M2Renderer::unloadModel(uint32_t modelId) {
    auto it = models.find(modelId);
    if (it == models.end()) return;
    destroyModelGPU(modelId, it->second);
    models.erase(it);
    modelUnusedSince_.erase(modelId);
    pinnedModelIds_.erase(modelId);
    reapedModelIds_.push_back(modelId);
}

TextureRef M2Renderer::loadTexture(const std::string& path, bool* loading) {
    if (loading) *loading = false;
    const std::string key = normalizeTexturePath(path);

    // The shared cache, or a worker that reads, uploads and puts it there
    // (TextureLoads): a texture a building or the ground loaded is the
    // same image here, and the frame never reads or uploads one. Whether the
    // texture actually uses alpha, and whether that alpha is a silhouette, is
    // read from its blocks there once (cacheStoredTexture). No black key from
    // the texture's name: every texture whose file name held "lamp", "fire",
    // "glow", "candle" and the like had its dark pixels discarded - 975
    // batches, the client keying none of them.
    //
    // The image's own sampler is the cache's. A batch samples it through a
    // sampler built from its own wrap flags and the sky's mip bias
    // (batchSampler), since other models and renderers share the image.
    if (!textureLoads_) {
        LOG_FATAL("M2Renderer asked for texture '", path, "' with no TextureLoads to make it: the "
                  "renderer that owns it must set one before it loads a model");
        std::abort();
    }
    TextureRef out;
    const TextureLoads::State state = textureLoads_->requestStored(key, out);
    if (state == TextureLoads::State::Resident) return out;
    if (state == TextureLoads::State::Loading) {
        if (loading) *loading = true;
        return whiteTexture_;
    }
    if (loggedTextureLoadFails_.insert(key).second) {
        LOG_WARNING("M2: Failed to load texture: ", path, " - its batches stay hidden until a retry finds it");
    }
    return whiteTexture_;
}

uint64_t M2Renderer::modelDataBytes() const {
    using pipeline::heapBytes;
    using pipeline::vectorBytes;
    const auto track = [](const pipeline::M2AnimationTrack& t) { return heapBytes(t); };
    uint64_t total = 0;
    for (const auto& [id, mResource] : models) {
        auto& m = *mResource;
        total += sizeof(M2ModelGPU);
        total += vectorBytes(m.bones, [](const pipeline::M2Bone& b) { return heapBytes(b); });
        total += vectorBytes(m.sequences) + vectorBytes(m.globalSequenceDurations);
        total += vectorBytes(m.particleEmitters,
                             [](const pipeline::M2ParticleEmitter& e) { return heapBytes(e); });
        total += vectorBytes(m.ribbonEmitters,
                             [](const pipeline::M2RibbonEmitter& e) { return heapBytes(e); });
        total += vectorBytes(m.textureTransforms,
                             [](const pipeline::M2TextureTransform& t) { return heapBytes(t); });
        total += vectorBytes(m.colorAlphaTracks, track) + vectorBytes(m.textureWeightTracks, track);
        total += vectorBytes(m.lights, [](const pipeline::M2Light& l) { return heapBytes(l); });
        total += vectorBytes(m.batches) + vectorBytes(m.materialDrawOrder);
        // The collision shape is the collision world's to count: it is
        // shared, and outlives the model while a placement holds it.
    }
    return total;
}

uint64_t M2Renderer::instanceBytes() const {
    using pipeline::vectorBytes;
    uint64_t total = scene_->instances.storageBytes();
    for (const auto& inst : scene_->instances) {
        total += vectorBytes(inst.boneMatrices) + vectorBytes(inst.particleRanges) + vectorBytes(inst.ribbonRanges);
    }
    return total;
}

uint32_t M2Renderer::getTotalTriangleCount() const {
    uint32_t total = 0;
    for (const auto& instance : scene_->instances) {
        if (instance.cachedModel) {
            total += instance.cachedModel->indexCount / 3;
        }
    }
    return total;
}

void M2Renderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layouts)
    destroy(device, opaquePipeline_);
    destroy(device, cutoutPipeline_);
    destroy(device, alphaTestPipeline_);
    destroyBlendedPipelines(scenePipes_);
    // Rebuilt by the renderer once it has decided the water's pass again: the
    // one these were built for may not survive the change that got us here.
    setAfterWaterPass(VK_NULL_HANDLE, VK_SAMPLE_COUNT_1_BIT);

    // The same main-pass pipelines initialize() builds, built by the same
    // function. The layouts are untouched above, so it makes none.
    buildMainPassPipelines(perFrameLayout_);

    core::Logger::getInstance().info("M2Renderer: pipelines recreated");
}

} // namespace rendering
} // namespace wowee
