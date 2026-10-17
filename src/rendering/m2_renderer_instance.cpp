#include <cstdlib>
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_model_classifier.hpp"
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

// Thread-local scratch buffers for collision queries (moved from header to
// avoid inline thread_local TLS init linker errors on Windows ARM64 / LLD).
namespace m2_internal {
thread_local std::vector<size_t> tl_m2_candidateScratch;
thread_local std::unordered_set<uint32_t> tl_m2_candidateIdScratch;
thread_local std::vector<uint32_t> tl_m2_collisionTriScratch;
} // namespace m2_internal

void M2Renderer::fileInCullGrid(const M2Instance& instance) {
    // The margin the shadow pass inflates a position by, before its division
    // by the pass's radius: the model's bound radius at this scale, times 1.5.
    const float boundRadius = instance.cachedModel ? instance.cachedModel->boundRadius
                                                   : instance.cachedBoundRadius;
    scene_->cullGrid_.update(instance.id, M2CullGrid::Entry{
        .position = instance.position,
        .cullCenter = instance.cachedCullCenter,
        .cullRadius = instance.cachedPaddedRadius,
        .shadowRadius = boundRadius * instance.scale * 1.5f});
}

void M2Renderer::gatherViewCandidates(const glm::vec3& camPos, const Frustum& frustum,
                                      float maxPossibleDistSq, std::vector<uint32_t>& out) const {
    out.clear();
    for (const auto& cell : scene_->cullGrid_.cells()) {
        if (cell.ids.empty()) continue;
        // Nothing in a column is drawn further away than this, and nothing
        // whose cull sphere is outside the frustum - and the column's box
        // holds every position and every sphere filed in it.
        if (distanceSqToBox(camPos, cell.viewMin, cell.viewMax) > maxPossibleDistSq) continue;
        if (!frustum.intersectsAABB(cell.viewMin, cell.viewMax)) continue;
        for (const uint32_t id : cell.ids) {
            const auto it = scene_->instanceIndexById.find(id);
            if (it != scene_->instanceIndexById.end() && it->second < scene_->instances.size()) {
                out.push_back(static_cast<uint32_t>(it->second));
            }
        }
    }
}

void M2Renderer::gatherShadowCandidates(const glm::mat4& lightSpace, float shadowRadius,
                                        std::vector<uint32_t>& out) const {
    out.clear();
    const float invRadius = shadowRadius > 0.0f ? 1.0f / shadowRadius : 0.0f;
    for (const auto& cell : scene_->cullGrid_.cells()) {
        if (cell.ids.empty()) continue;
        // The light's box, not the camera's: a caster behind the camera can
        // still throw its shadow into view.
        if (!cellMayCastShadow(lightSpace, cell.posMin, cell.posMax,
                               cell.maxShadowRadius * invRadius)) continue;
        for (const uint32_t id : cell.ids) {
            const auto it = scene_->instanceIndexById.find(id);
            if (it != scene_->instanceIndexById.end() && it->second < scene_->instances.size()) {
                out.push_back(static_cast<uint32_t>(it->second));
            }
        }
    }
}

void M2Renderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return;
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];

    // The box the instance is filed under, which is what it has to be taken
    // out of once it has moved.
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;

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
    fileInCullGrid(inst);

    // Incrementally update spatial grid
    refileBounds(scene_->spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
}

void M2Renderer::setInstanceFade(uint32_t instanceId, float alpha) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    scene_->instances[idxIt->second].fade = std::clamp(alpha, 0.0f, 1.0f);
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
        inst.animTime = 0.0f;  // Reset to bind pose
    }
}

void M2Renderer::restartInstanceAnimation(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    inst.animTime = 0.0f;
    if (inst.cachedModel) computeBoneMatrices(*inst.cachedModel, inst, &cachedView_);
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
            inst.animTime = inst.animDuration;
            inst.animSpeed = 0.0f;
        } else {
            inst.animTime = 0.0f;
            inst.animSpeed = 1.0f;
        }
        return;
    }
}

namespace {

// Put one planned animation on an instance. False when the model lacks it.
bool startPlannedAnimation(M2Instance& inst, const M2AnimationPlay& play) {
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
            inst.animTime = play.frozen ? start : end;
            inst.animSpeed = 0.0f;
            inst.holdAtEnd = true;
        } else {
            inst.animTime = start;
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
    if (!startPlannedAnimation(inst, now)) return false;
    inst.nextAnimation = now.end == M2AnimationPlay::End::Next ? next : std::nullopt;
    return true;
}

void M2Renderer::advanceToNextAnimation(M2Instance& inst) {
    const M2AnimationPlay next = *inst.nextAnimation;
    inst.nextAnimation.reset();
    if (!startPlannedAnimation(inst, next)) {
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
            inst.animTime = 0.0f;
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

float M2Renderer::getInstanceAnimDuration(uint32_t instanceId) const {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return 0.0f;
    const auto& inst = scene_->instances[idxIt->second];
    if (!inst.cachedModel) return 0.0f;
    const auto& seqs = inst.cachedModel->sequences;
    if (seqs.empty()) return 0.0f;
    int seqIdx = inst.currentSequenceIndex;
    if (seqIdx < 0 || seqIdx >= static_cast<int>(seqs.size())) seqIdx = 0;
    return seqs[seqIdx].duration; // in milliseconds
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
    // and the spatial-grid bounds, leaving stale grid cells pointing at
    // a NaN-bounded instance.
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(transform[c][r])) return;
    auto& inst = scene_->instances[idxIt->second];

    // Remove old grid cells before updating bounds
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;
    const glm::vec3 oldPosition = inst.position;

    // Update model matrix directly
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    // Extract position from transform for bounds
    inst.position = glm::vec3(transform[3]);

    // The dedup map is keyed on position, so it has to move with the instance.
    // It did not, and only rebuildSpatialIndex ever put it right - which this
    // path deliberately avoids. A ship's doodads are created at the origin and
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
    fileInCullGrid(inst);

    // Incrementally update spatial grid (remove old cells, add new cells)
    refileBounds(scene_->spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
    // No spatialIndexDirty_ = true - handled incrementally
}

void M2Renderer::removeInstance(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    size_t idx = idxIt->second;
    if (idx >= scene_->instances.size()) return;

    auto& inst = scene_->instances[idx];

    // Remove from spatial grid incrementally (same pattern as the move-update path)
    eraseBounds(scene_->spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
    scene_->cullGrid_.remove(instanceId);

    // Remove from dedup map
    if (!inst.cachedIsGroundDetail) {
        DedupKey dk{.modelId = inst.modelId,
                    .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
        scene_->instanceDedupMap_.erase(dk);
    }

    destroyInstanceBones(inst, /*defer=*/true);

    // Swap-remove: move last element to the hole and pop_back to avoid O(n) shift
    scene_->instanceIndexById.erase(instanceId);
    if (idx < scene_->instances.size() - 1) {
        uint32_t movedId = scene_->instances.back().id;
        scene_->instances[idx] = std::move(scene_->instances.back());
        scene_->instances.pop_back();
        scene_->instanceIndexById[movedId] = idx;
    } else {
        scene_->instances.pop_back();
    }

    rebuildInstanceIndexLists();
}

void M2Renderer::rebuildInstanceIndexLists() {
    // Rebuild the lightweight auxiliary index vectors (animated, particle, light).
    // These are small vectors of indices that are rebuilt cheaply.
    scene_->animatedInstanceIndices_.clear();
    scene_->particleOnlyInstanceIndices_.clear();
    scene_->particleInstanceIndices_.clear();
    scene_->lightInstanceIndices_.clear();
    for (size_t i = 0; i < scene_->instances.size(); i++) {
        auto& ri = scene_->instances[i];
        if (ri.cachedHasEmitters) scene_->particleInstanceIndices_.push_back(i);
        if (ri.cachedHasAnimation && !ri.cachedDisableAnimation)
            scene_->animatedInstanceIndices_.push_back(i);
        else if (ri.cachedHasEmitters)
            scene_->particleOnlyInstanceIndices_.push_back(i);
        if (ri.cachedModel && ri.cachedModel->canThrowLocalLight())
            scene_->lightInstanceIndices_.push_back(i);
    }
}

void M2Renderer::setInstanceIsGameObject(uint32_t instanceId, bool isGameObject) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return;
    scene_->instances[idxIt->second].isGameObject = isGameObject;
}

void M2Renderer::setSkipCollision(uint32_t instanceId, bool skip) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return;
    scene_->instances[idxIt->second].skipCollision = skip;
}

void M2Renderer::setSkipWallCollision(uint32_t instanceId, bool skip) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return;
    scene_->instances[idxIt->second].skipWallCollision = skip;
}

void M2Renderer::setInteriorLight(uint32_t instanceId, const std::optional<InteriorLight>& light) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return;
    scene_->instances[idxIt->second].interiorLight = packInteriorLight(light);
}

void M2Renderer::finishInstance(uint32_t instanceId) {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end()) return;
    auto& inst = scene_->instances[idxIt->second];
    if (inst.finishing) return;
    inst.finishing = true;
    // Its particles have at most their longest life left to run.
    float longestLife = 0.0f;
    if (inst.cachedModel) {
        for (const auto& em : inst.cachedModel->particleEmitters) {
            longestLife = std::max(longestLife,
                interpFloat(em.lifespan, inst.animTime, inst.globalSequenceTime,
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
        const bool ribbonsLeft = std::any_of(inst.ribbons.begin(), inst.ribbons.end(),
                                             [](const auto& r) { return !r.edges.empty(); });
        // Gone when nothing it emitted is left - or when its particles have
        // had their whole life, though it was out of sight and not updated.
        if ((inst.particles.empty() && !ribbonsLeft) || now >= f.deadline) {
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

    // A tile's doodads leave together, and this used to compact the array and
    // then rebuild the whole spatial index - every grid cell, the id map and
    // the dedup map, for every instance still in the world. That was the
    // 70-130 ms hitch each tile unload cost while moving. Only what the
    // leaving instances own is touched now: their cells, their dedup keys and
    // the slots they vacate. The grid files ids, not indices, so a survivor
    // moved into a vacated slot keeps its cells as they are.
    std::unordered_set<uint32_t> toRemove;
    toRemove.reserve(instanceIds.size());
    std::unordered_set<GridCell, GridCellHash> touchedCells;
    std::vector<size_t> holes;
    holes.reserve(instanceIds.size());
    for (uint32_t id : instanceIds) {
        auto idxIt = scene_->instanceIndexById.find(id);
        if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) continue;
        if (!toRemove.insert(id).second) continue;
        auto& inst = scene_->instances[idxIt->second];
        holes.push_back(idxIt->second);

        const GridCell minCell = toSpatialCell(inst.worldBoundsMin);
        const GridCell maxCell = toSpatialCell(inst.worldBoundsMax);
        for (int z = minCell.z; z <= maxCell.z; z++)
            for (int y = minCell.y; y <= maxCell.y; y++)
                for (int x = minCell.x; x <= maxCell.x; x++)
                    touchedCells.insert(GridCell{.x = x, .y = y, .z = z});

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
        destroyInstanceBones(inst, /*defer=*/true);
        scene_->cullGrid_.remove(id);
        scene_->instanceIndexById.erase(idxIt);
    }
    if (holes.empty()) return;

    for (const GridCell& cell : touchedCells) {
        auto it = scene_->spatialGrid.find(cell);
        if (it == scene_->spatialGrid.end()) continue;
        std::erase_if(it->second, [&](uint32_t id) { return toRemove.count(id) != 0; });
        if (it->second.empty()) scene_->spatialGrid.erase(it);
    }

    // Fill the holes from the back, highest first, so a slot being filled is
    // never one still waiting to be emptied: everything past the highest
    // remaining hole is a survivor.
    std::sort(holes.begin(), holes.end(), std::greater<>());
    for (size_t hole : holes) {
        const size_t last = scene_->instances.size() - 1;
        if (hole != last) {
            scene_->instances[hole] = std::move(scene_->instances[last]);
            scene_->instanceIndexById[scene_->instances[hole].id] = hole;
        }
        scene_->instances.pop_back();
    }
    rebuildInstanceIndexLists();
}

void M2Renderer::clear() {
    if (vkCtx_) {
        // Their owners see them gone (hasPosedEmitters) and ask again.
        destroyPosedEmitterModels();
        for (auto& [id, model] : models) {
            destroyModelGPU(model);
        }
        for (auto& inst : scene_->instances) {
            destroyInstanceBones(inst);
        }
        // Each retired model/instance returns its own sets after the fences.
        // Keep the pools and persistent glow/dummy/mega-bone sets alive.
    }
    models.clear();
    pinnedModelIds_.clear();
    scene_->instances.clear();
    scene_->spatialGrid.clear();
    scene_->instanceIndexById.clear();
    scene_->cullGrid_.clear();
    scene_->boneSeedInstanceByModel_.clear();
    scene_->instanceDedupMap_.clear();
    for (auto& ids : scene_->cullSubmittedIds_) ids.clear();
    for (auto& ids : scene_->cullReadableIds_) ids.clear();
    scene_->animatedInstanceIndices_.clear();
    scene_->particleOnlyInstanceIndices_.clear();
    scene_->particleInstanceIndices_.clear();
    scene_->lightInstanceIndices_.clear();

    // The models' textures went back to the shared cache with the models,
    // where nothing references them and eviction makes room from them first.
    // The retry timers go too, so the next character or map reads what failed
    // here afresh instead of waiting them out.
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
}

void M2Renderer::clearInstances() {
    for (auto& inst : scene_->instances) destroyInstanceBones(inst);
    *scene_ = InstanceState{};
}

void M2Renderer::setCollisionFocus(const glm::vec3& worldPos, float radius) {
    collisionFocus.set(worldPos, radius);
}

void M2Renderer::resetQueryStats() {
    queryTimeMs = 0.0;
    queryCallCount = 0;
}

void M2Renderer::rebuildSpatialIndex() {
    scene_->spatialGrid.clear();
    scene_->instanceIndexById.clear();
    scene_->boneSeedInstanceByModel_.clear();
    scene_->instanceDedupMap_.clear();
    scene_->instanceIndexById.reserve(scene_->instances.size());
    scene_->animatedInstanceIndices_.clear();
    scene_->particleOnlyInstanceIndices_.clear();
    scene_->particleInstanceIndices_.clear();
    scene_->lightInstanceIndices_.clear();

    for (size_t i = 0; i < scene_->instances.size(); i++) {
        auto& inst = scene_->instances[i];
        scene_->instanceIndexById[inst.id] = i;

        // Re-cache model pointer (may have changed after model map modifications)
        auto mdlIt = models.find(inst.modelId);
        inst.cachedModel = (mdlIt != models.end()) ? &mdlIt->second : nullptr;

        // Rebuild dedup map (skip ground detail)
        if (!inst.cachedIsGroundDetail) {
            DedupKey dk{.modelId = inst.modelId,
                        .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                        .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                        .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
            scene_->instanceDedupMap_[dk] = inst.id;
        }

        if (inst.cachedHasEmitters) {
            scene_->particleInstanceIndices_.push_back(i);
        }
        if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
            scene_->animatedInstanceIndices_.push_back(i);
        } else if (inst.cachedHasEmitters) {
            scene_->particleOnlyInstanceIndices_.push_back(i);
        }
        if (inst.cachedModel && inst.cachedModel->canThrowLocalLight()) {
            scene_->lightInstanceIndices_.push_back(i);
        }

        insertBounds(scene_->spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, inst.id);
    }
    scene_->cullGrid_.clear();
    for (const auto& inst : scene_->instances) fileInCullGrid(inst);
    scene_->spatialIndexDirty_ = false;
}

void M2Renderer::gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax,
                                  std::vector<size_t>& outIndices) const {
    outIndices.clear();

    gatherIds(scene_->spatialGrid, queryMin, queryMax, tl_m2_candidateIdScratch,
              [&](uint32_t id) {
                  auto idxIt = scene_->instanceIndexById.find(id);
                  if (idxIt != scene_->instanceIndexById.end()) {
                      outIndices.push_back(idxIt->second);
                  }
              });

    // Safety fallback to preserve collision correctness if the spatial index
    // misses candidates (e.g. during streaming churn).
    if (outIndices.empty() && !scene_->instances.empty()) {
        outIndices.reserve(scene_->instances.size());
        for (size_t i = 0; i < scene_->instances.size(); i++) {
            outIndices.push_back(i);
        }
    }
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
    for (const auto& [id, model] : models) {
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
            destroyModelGPU(it->second);
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
    destroyModelGPU(it->second);
    models.erase(it);
    modelUnusedSince_.erase(modelId);
    pinnedModelIds_.erase(modelId);
    reapedModelIds_.push_back(modelId);
}

TextureRef M2Renderer::loadTexture(const std::string& path) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    const std::string key = normalizeTexturePath(path);
    const uint64_t lookupSerial = ++textureLookupSerial_;

    // The shared cache: a texture a building or the ground loaded is the same
    // image here.
    if (TextureRef cached = vkCtx_->textures().find({key, TextureForm::Stored, 0})) return cached;
    auto failIt = failedTextureRetryAt_.find(key);
    if (failIt != failedTextureRetryAt_.end() && lookupSerial < failIt->second) {
        return whiteTexture_;
    }

    // Check pre-decoded BLP cache first (populated by background worker threads)
    pipeline::BLPImage blp;
    if (predecodedBLPCache_) {
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            blp = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!blp.isValid()) {
        // M2 skins are sampled and their transparency is read from the
        // blocks, so they need no decode.
        blp = assetManager->loadTexture(key, true);
    }
    if (!blp.isValid()) {
        // Cache misses briefly to avoid repeated expensive MPQ/disk probes.
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        if (loggedTextureLoadFails_.insert(key).second) {
            LOG_WARNING("M2: Failed to load texture: ", path, " - its batches stay hidden until a retry finds it");
        }
        return whiteTexture_;
    }

    // Into the shared cache, with whether the texture actually uses alpha and
    // whether that alpha is a silhouette, read from the blocks once
    // (cacheStoredTexture). No black key from the texture's name: every
    // texture whose file name held "lamp", "fire", "glow", "candle" and the
    // like had its dark pixels discarded - 975 batches, the client keying
    // none of them.
    //
    // The image's own sampler is the cache's. A batch samples it through a
    // sampler built from its own wrap flags and the sky's mip bias
    // (batchImageInfo), since other models and renderers share the image.
    TextureRef uploaded = cacheStoredTexture(*vkCtx_, key, blp);
    if (!uploaded) {
        // Refused or failed, as the cache has said; asked for again after the
        // retry interval rather than decoded every frame meanwhile.
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        return whiteTexture_;
    }
    failedTextureRetryAt_.erase(key);
    LOG_DEBUG("M2: Loaded texture: ", path, " (", blp.width, "x", blp.height, ")");
    return uploaded;
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

void M2Renderer::debugDumpFloorCandidatesAt(float glX, float glY, float glZ) const {
    LOG_WARNING("=== M2 Floor Debug at render(", glX, ", ", glY, ", ", glZ, ") ===");
    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);
    int reported = 0;
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = scene_->instances[idx];
        const char* rejected = nullptr;
        if (collisionFocus.excludes(instance.worldBoundsMin, instance.worldBoundsMax)) {
            rejected = "outside the collision focus";
        } else if (!instance.cachedModel) {
            rejected = "no model";
        } else if (instance.scale <= 0.001f) {
            rejected = "zero scale";
        } else if (instance.skipCollision) {
            rejected = "skipCollision";
        }
        const M2ModelGPU* model = instance.cachedModel;
        const bool authored = model && model->collision.valid();
        if (!rejected && model) {
            if (!authored) rejected = "no collision mesh";
            else if (model->isInvisibleTrap || model->isSpellEffect) rejected = "an effect or a trap";
        }
        if (++reported > 12) { LOG_WARNING("  ... and more"); break; }
        LOG_WARNING("  '", model ? model->name : std::string("?"),
                    "' isGameObject=", instance.isGameObject ? 1 : 0,
                    " authoredCollision=", authored ? 1 : 0,
                    " tris=", authored ? model->collision.triCount : 0u,
                    " bounds z ", instance.worldBoundsMin.z, "..", instance.worldBoundsMax.z,
                    (rejected ? "  REJECTED: " : "  considered"), (rejected ? rejected : ""));
    }
    if (reported == 0) {
        LOG_WARNING("  nothing at all - no doodad instance overlaps this spot,"
                    " so a deck underfoot is not in the spatial grid here");
    }
    LOG_WARNING("=== Total: ", reported, " M2 candidates ===");
}

bool M2Renderer::getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin,
                                        glm::vec3& outMax) const {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) return false;
    const auto& inst = scene_->instances[idxIt->second];
    if (!inst.cachedModel) return false;
    outMin = inst.worldBoundsMin;
    outMax = inst.worldBoundsMax;
    return true;
}

std::optional<float> M2Renderer::getInstanceFloorHeight(uint32_t instanceId,
                                                       float glX, float glY,
                                                       float glZ) const {
    auto idxIt = scene_->instanceIndexById.find(instanceId);
    if (idxIt == scene_->instanceIndexById.end() || idxIt->second >= scene_->instances.size()) {
        return std::nullopt;
    }
    const auto& instance = scene_->instances[idxIt->second];
    if (!instance.cachedModel || instance.scale <= 0.001f) return std::nullopt;
    const M2ModelGPU& model = *instance.cachedModel;
    if (!model.collision.valid()) return std::nullopt;

    // Same cast as getFloorHeight: world-down through the instance transform,
    // so a car placed with any pitch still sees a ray along gravity.
    const glm::vec3 localRayOrigin = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
    const glm::vec3 localRayEnd = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
    const glm::vec3 localRayVector = localRayEnd - localRayOrigin;
    const float localRayLength = glm::length(localRayVector);
    if (localRayLength <= 1e-5f) return std::nullopt;
    const glm::vec3 localRayDir = localRayVector / localRayLength;

    model.collision.getFloorTrisInRange(
        std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
        std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
        std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
        std::max(localRayOrigin.y, localRayEnd.y) + 1.0f,
        tl_m2_collisionTriScratch);

    std::optional<float> best;
    for (uint32_t ti : tl_m2_collisionTriScratch) {
        if (ti >= model.collision.triCount) continue;
        const auto& verts = model.collision.vertices;
        const auto& idx = model.collision.indices;
        const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir,
                                                verts[idx[ti * 3]],
                                                verts[idx[ti * 3 + 1]],
                                                verts[idx[ti * 3 + 2]]);
        if (tHit < 0.0f || tHit > localRayLength) continue;
        const glm::vec3 worldHit = glm::vec3(
            instance.modelMatrix * glm::vec4(localRayOrigin + localRayDir * tHit, 1.0f));
        // At or under the probe, and the highest such - the deck rather than
        // whatever structure the car carries beneath it.
        if (worldHit.z <= glZ && (!best || worldHit.z > *best)) best = worldHit.z;
    }
    return best;
}

std::optional<float> M2Renderer::getFloorHeight(float glX, float glY, float glZ, float* outNormalZ) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;  // Default to flat

    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = scene_->instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        if (!instance.cachedModel) continue;
        if (instance.scale <= 0.001f) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // A doodad is stood on by its collision mesh and by nothing else, as
        // in the client: a model that ships none - a bush, a sign, grass - is
        // not there to collide with. The floor used to fall back to the top
        // of a box shrunk from the model's bounds by rules keyed on its name
        // (bridge, planter, statue, fountain), which put floors on top of
        // things the client walks through.
        if (!model.collision.valid() || model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision) continue;

        // Vertical ray against the collision triangles.
        {
            // Cast world-down through the instance transform. Using local -Z
            // only works for yaw-only placements; pitched flat models (notably
            // Exodarplatform01, placed as the Azuremyst flight-master ramp)
            // otherwise see a ray that is diagonal relative to world gravity.
            const glm::vec3 localRayOrigin = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
            const glm::vec3 localRayEnd = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
            glm::vec3 localRayVector = localRayEnd - localRayOrigin;
            const float localRayLength = glm::length(localRayVector);
            if (localRayLength <= 1e-5f) continue;
            const glm::vec3 localRayDir = localRayVector / localRayLength;

            model.collision.getFloorTrisInRange(
                std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
                std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
                std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
                std::max(localRayOrigin.y, localRayEnd.y) + 1.0f,
                tl_m2_collisionTriScratch);

            float bestWorldHitZ = -std::numeric_limits<float>::max();
            float bestHitNormalZ = 1.0f;
            bool hitAny = false;

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                // The intersection is already two-sided, so the reversed
                // winding this used to retry answers the same thing.
                const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2);
                if (tHit < 0.0f || tHit > localRayLength) continue;

                const glm::vec3 localHit = localRayOrigin + localRayDir * tHit;
                const glm::vec3 worldHit = glm::vec3(
                    instance.modelMatrix * glm::vec4(localHit, 1.0f));

                // Walkable normal check (world space)
                glm::vec3 worldN(0.0f, 0.0f, 1.0f);  // Default to flat
                glm::vec3 localN = glm::cross(v1 - v0, v2 - v0);
                float nLen = glm::length(localN);
                if (nLen > 0.001f) {
                    localN /= nLen;
                    if (localN.z < 0.0f) localN = -localN;
                    glm::vec3 transformedN = glm::vec3(
                        instance.modelMatrix * glm::vec4(localN, 0.0f));
                    float wnLen = glm::length(transformedN);
                    if (wnLen > 0.001f) {
                        worldN = transformedN / wnLen;
                    } // else: keep worldN = (0,0,1) flat default
                    if (std::abs(worldN.z) < 0.35f) continue; // too steep (~70° max slope)
                }

                if (worldHit.z <= glZ + 3.0f && worldHit.z > bestWorldHitZ) {
                    bestWorldHitZ = worldHit.z;
                    hitAny = true;
                    bestHitNormalZ = std::abs(worldN.z);
                }
            }

            if (hitAny) {
                if (!bestFloor || bestWorldHitZ > *bestFloor) {
                    bestFloor = bestWorldHitZ;
                    bestNormalZ = bestHitNormalZ;
                }
            }
        }
    }

    // Output surface normal if requested
    if (outNormalZ) {
        *outNormalZ = bestNormalZ;
    }

    return bestFloor;
}

bool M2Renderer::checkCollision(const glm::vec3& from, const glm::vec3& to,
                                 glm::vec3& adjustedPos, float playerRadius) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    adjustedPos = to;
    bool collided = false;

    glm::vec3 queryMin = glm::min(from, to) - glm::vec3(7.0f, 7.0f, 5.0f);
    glm::vec3 queryMax = glm::max(from, to) + glm::vec3(7.0f, 7.0f, 5.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    // Check against all M2 instances in local space (rotation-aware).
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = scene_->instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        const float broadMargin = playerRadius + 1.0f;
        if (from.x < instance.worldBoundsMin.x - broadMargin && adjustedPos.x < instance.worldBoundsMin.x - broadMargin) continue;
        if (from.x > instance.worldBoundsMax.x + broadMargin && adjustedPos.x > instance.worldBoundsMax.x + broadMargin) continue;
        if (from.y < instance.worldBoundsMin.y - broadMargin && adjustedPos.y < instance.worldBoundsMin.y - broadMargin) continue;
        if (from.y > instance.worldBoundsMax.y + broadMargin && adjustedPos.y > instance.worldBoundsMax.y + broadMargin) continue;
        if (from.z > instance.worldBoundsMax.z + 2.5f && adjustedPos.z > instance.worldBoundsMax.z + 2.5f) continue;
        if (from.z + 2.5f < instance.worldBoundsMin.z && adjustedPos.z + 2.5f < instance.worldBoundsMin.z) continue;

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // Only a collision mesh blocks, as in the client. A model without one
        // used to be stopped by a box made from its bounds and shaped by its
        // name - a tree's trunk, a crate, a narrow post.
        if (!model.collision.valid() || model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision || instance.skipWallCollision) continue;
        if (instance.scale <= 0.001f) continue;

        // Closest-point push against the collision triangles.
        {
            glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
            glm::vec3 localPos  = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
            float localRadius = playerRadius / instance.scale;

            model.collision.getWallTrisInRange(
                std::min(localFrom.x, localPos.x) - localRadius - 1.0f,
                std::min(localFrom.y, localPos.y) - localRadius - 1.0f,
                std::max(localFrom.x, localPos.x) + localRadius + 1.0f,
                std::max(localFrom.y, localPos.y) + localRadius + 1.0f,
                tl_m2_collisionTriScratch);

            constexpr float PLAYER_HEIGHT = 2.0f;
            constexpr float MAX_TOTAL_PUSH = 0.02f; // Cap total push per instance
            bool pushed = false;
            float totalPushX = 0.0f, totalPushY = 0.0f;

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;
                if (localPos.z + PLAYER_HEIGHT < model.collision.triBounds[ti].minZ ||
                    localPos.z > model.collision.triBounds[ti].maxZ) continue;

                // Step-up: only skip wall when player is rising (jumping over it)
                constexpr float MAX_STEP_UP = 1.2f;
                bool rising = (localPos.z > localFrom.z + 0.05f);
                if (rising && localPos.z + MAX_STEP_UP >= model.collision.triBounds[ti].maxZ) continue;

                // Early out if we already pushed enough this instance
                float totalPushSoFar = std::sqrt(totalPushX * totalPushX + totalPushY * totalPushY);
                if (totalPushSoFar >= MAX_TOTAL_PUSH) break;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                glm::vec3 closest = closestPointOnTriangle(localPos, v0, v1, v2);
                glm::vec3 diff = localPos - closest;
                float distXY = std::sqrt(diff.x * diff.x + diff.y * diff.y);

                if (distXY < localRadius && distXY > 1e-4f) {
                    // Gentle push - very small fraction of penetration
                    float penetration = localRadius - distXY;
                    float pushDist = std::clamp(penetration * 0.08f, 0.001f, 0.015f);
                    float dx = (diff.x / distXY) * pushDist;
                    float dy = (diff.y / distXY) * pushDist;
                    localPos.x += dx;
                    localPos.y += dy;
                    totalPushX += dx;
                    totalPushY += dy;
                    pushed = true;
                } else if (distXY < 1e-4f) {
                    // On the plane - soft push along triangle normal XY
                    glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
                    float nxyLen = std::sqrt(n.x * n.x + n.y * n.y);
                    if (nxyLen > 1e-4f) {
                        float pushDist = std::min(localRadius, 0.015f);
                        float dx = (n.x / nxyLen) * pushDist;
                        float dy = (n.y / nxyLen) * pushDist;
                        localPos.x += dx;
                        localPos.y += dy;
                        totalPushX += dx;
                        totalPushY += dy;
                        pushed = true;
                    }
                }
            }

            if (pushed) {
                glm::vec3 worldPos = glm::vec3(instance.modelMatrix * glm::vec4(localPos, 1.0f));
                adjustedPos.x = worldPos.x;
                adjustedPos.y = worldPos.y;
                collided = true;
                // Which doodad is in the way, once per model.
                //
                // A model that blocks unexpectedly is reported as "I am
                // stuck on this", and the model's name is the one thing
                // needed to look at its collision mesh.
                //
                // Once per distinct model, not per frame: walking into
                // something touches it every frame for as long as you lean on
                // it.
                static std::set<std::string> saidBlocked;
                if (!model.name.empty() && saidBlocked.insert(model.name).second) {
                    LOG_INFO("Collision: blocked by '", model.name,
                             "' (its collision mesh)");
                }
            }
        }
    }

    return collided;
}

float M2Renderer::raycastCollision(const glm::vec3& origin, const glm::vec3& direction,
                                   float maxDistance) const {
    // The client's world line test with doodad collision (flag 0x1, render
    // spec 6c): the segment against each doodad's collision triangles. It
    // used to be against each doodad's bounding box, so the camera stopped at
    // the box around a tree's crown or a bush that has nothing to collide
    // with, and skipped doodads under about 0.75 yd outright.
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;

    const glm::vec3 rayEnd = origin + direction * maxDistance;
    const glm::vec3 queryMin = glm::min(origin, rayEnd) - glm::vec3(1.0f);
    const glm::vec3 queryMax = glm::max(origin, rayEnd) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = scene_->instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin, instance.worldBoundsMax)) continue;
        if (!instance.cachedModel || instance.scale <= 0.001f) continue;
        const M2ModelGPU& model = *instance.cachedModel;
        if (!model.collision.valid() || model.isInvisibleTrap || model.isSpellEffect) continue;

        float tEnter = 0.0f;
        if (!segmentIntersectsAABB(origin, origin + direction * closestHit,
                                   instance.worldBoundsMin, instance.worldBoundsMax, tEnter)) {
            continue;
        }

        // In model space; the segment's parameter is the same in both.
        const glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        const glm::vec3 localEnd = glm::vec3(
            instance.invModelMatrix * glm::vec4(origin + direction * closestHit, 1.0f));
        const glm::vec3 localVector = localEnd - localOrigin;
        const float localLength = glm::length(localVector);
        if (localLength <= 1e-5f) continue;
        const glm::vec3 localDir = localVector / localLength;

        const auto& verts = model.collision.vertices;
        const auto& indices = model.collision.indices;
        float nearest = localLength;
        for (uint32_t ti = 0; ti < model.collision.triCount; ++ti) {
            const float t = rayTriangleIntersect(localOrigin, localDir, verts[indices[ti * 3]],
                                                 verts[indices[ti * 3 + 1]],
                                                 verts[indices[ti * 3 + 2]]);
            if (t >= 0.0f && t < nearest) nearest = t;
        }
        closestHit *= nearest / localLength;
    }
    return closestHit;
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

void M2Renderer::collectGrassClearings(float minX, float minY, float maxX, float maxY,
                                       std::vector<pipeline::GrassClearingSource>& out) const {
    constexpr float kEase = 4.0f;
    for (const auto& inst : scene_->instances) {
        // The ground detail IS the grass's kin - clutter clearing the ground
        // around clutter would eat the whole field.
        if (inst.cachedIsGroundDetail) continue;

        // Footprint radius from the horizontal bounds, capped: a fence post
        // clears its foot, a wagon its wheelbase, and a tree its trunk - not
        // the shadow of its crown, or every forest would be bald.
        const float ex = inst.worldBoundsMax.x - inst.worldBoundsMin.x;
        const float ey = inst.worldBoundsMax.y - inst.worldBoundsMin.y;
        const float clearing = std::clamp(0.35f * std::max(ex, ey), 0.4f, 3.0f);
        const float reach = clearing + kEase;

        const float cx = 0.5f * (inst.worldBoundsMin.x + inst.worldBoundsMax.x);
        const float cy = 0.5f * (inst.worldBoundsMin.y + inst.worldBoundsMax.y);
        if (cx + reach < minX || cx - reach > maxX ||
            cy + reach < minY || cy - reach > maxY) {
            continue;
        }
        out.push_back({cx, cy, cx, cy, clearing, kEase});
    }
}

} // namespace rendering
} // namespace wowee
