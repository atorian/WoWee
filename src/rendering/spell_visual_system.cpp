#include "rendering/spell_visual_system.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/character_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/required_table.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/m2_model_cache.hpp"
#include "foundation/logger.hpp"
#include "foundation/thread_pool.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

void SpellVisualSystem::initialize(M2Renderer* m2Renderer, Renderer* renderer) {
    m2Renderer_ = m2Renderer;
    renderer_ = renderer;
}

void SpellVisualSystem::shutdown() {
    // The loads read through the asset manager, which goes away after this.
    for (auto& load : pendingLoads_) {
        if (load.future.valid()) load.future.wait();
    }
    pendingLoads_.clear();
    pendingModelIds_.clear();
    reset();
    m2Renderer_ = nullptr;
    renderer_ = nullptr;
    cachedAssetManager_ = nullptr;
}

// SpellVisual -> its precast, cast and impact kits; SpellVisualKit -> every
// effect column it fills; SpellVisualEffectName -> the model and its scale.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;

    if (!cachedAssetManager_) {
        cachedAssetManager_ = renderer_ ? renderer_->getAssetManager() : nullptr;
    }
    // Not an attempt. "Set early to prevent re-entry on failure" was the
    // intent, but there is no failure yet - only assets that have not
    // arrived. Latching here left spell visuals off for the session
    // whenever this was reached first. An asset manager that exists but has
    // not mounted its archives yet is the same case: loadDBC answers nothing.
    if (!cachedAssetManager_ || !cachedAssetManager_->isInitialized()) return;
    spellVisualDbcLoaded_ = true;  // a real attempt follows; do not repeat it

    const pipeline::GameTables& tables = renderer_->gameTables();

    // SpellVisualEffectName: id -> model, scale and its limits. The scale
    // columns are WotLK's; an expansion whose layout does not name them
    // draws every effect at its model's size.
    const pipeline::RequiredTable fxDbc(tables, "SpellVisualEffectName.dbc", "spell effect models");
    const uint32_t fxIdField = fxDbc.column("ID");
    const uint32_t fxPathField = fxDbc.column("FilePath");
    constexpr uint32_t kAbsent = pipeline::RequiredTable::kAbsent;
    const uint32_t fxScaleField = fxDbc.optionalColumn("Scale");
    const uint32_t fxMinField = fxDbc.optionalColumn("MinAllowedScale");
    const uint32_t fxMaxField = fxDbc.optionalColumn("MaxAllowedScale");
    const auto positive = [&](uint32_t row, uint32_t field, float fallback) {
        if (field == kAbsent) return fallback;
        const float v = fxDbc->getFloat(row, field);
        return std::isfinite(v) && v > 0.0f ? v : fallback;
    };
    std::unordered_map<uint32_t, SpellKitEffect> effects;
    for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
        const uint32_t id = fxDbc->getUInt32(i, fxIdField);
        std::string path = fxDbc->getString(i, fxPathField);
        if (!id || path.empty()) continue;
        SpellKitEffect effect;
        // The DBC stores the extension the art was authored with; what
        // shipped is .m2.
        effect.modelPath = pipeline::modelPathToM2(path);
        effect.scale = positive(i, fxScaleField, 1.0f);
        effect.minScale = positive(i, fxMinField, 0.0f);
        effect.maxScale = positive(i, fxMaxField, 0.0f);
        if (effect.maxScale > 0.0f && effect.maxScale < effect.minScale)
            std::swap(effect.minScale, effect.maxScale);
        effects.emplace(id, std::move(effect));
    }

    // SpellVisualKit: every column it fills, hung where that column hangs.
    // Each expansion's layout names the effect columns its file has.
    const pipeline::RequiredTable kitDbc(tables, "SpellVisualKit.dbc", "spell effect kits");
    const uint32_t kitIdField = kitDbc.column("ID");
    std::vector<uint32_t> columnFields;
    for (const auto& column : kSpellKitColumns)
        columnFields.push_back(kitDbc.optionalColumn(column.field));
    size_t missingEffects = 0;
    for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
        const uint32_t kitId = kitDbc->getUInt32(i, kitIdField);
        if (!kitId) continue;
        SpellVisualKit kit;
        for (size_t c = 0; c < std::size(kSpellKitColumns); ++c) {
            if (columnFields[c] == kAbsent) continue;
            const uint32_t effectId = kitDbc->getUInt32(i, columnFields[c]);
            if (!effectId) continue;
            const auto it = effects.find(effectId);
            if (it == effects.end()) {
                ++missingEffects;
                continue;
            }
            SpellKitEffect effect = it->second;
            effect.attachmentId = kSpellKitColumns[c].attachmentId;
            kit.effects.push_back(std::move(effect));
        }
        if (!kit.effects.empty()) kits_.emplace(kitId, std::move(kit));
    }
    if (missingEffects > 0) {
        LOG_WARNING("SpellVisual: ", missingEffects, " kit effect(s) name a SpellVisualEffectName row "
                    "that is not there; those effects do not show");
    }

    // SpellVisual: its three kits.
    const pipeline::RequiredTable svDbc(tables, "SpellVisual.dbc", "spell effects");
    const uint32_t svIdField = svDbc.column("ID");
    const uint32_t precastField = svDbc.column("PrecastKit");
    const uint32_t castField = svDbc.column("CastKit");
    const uint32_t impactField = svDbc.column("ImpactKit");
    const auto kitAt = [&](uint32_t record, uint32_t field) -> uint32_t {
        const uint32_t kitId = svDbc->getUInt32(record, field);
        return kits_.count(kitId) ? kitId : 0;
    };
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        const uint32_t visualId = svDbc->getUInt32(i, svIdField);
        if (!visualId) continue;
        const VisualKits kits{.precast = kitAt(i, precastField), .cast = kitAt(i, castField),
                              .impact = kitAt(i, impactField)};
        if (kits.precast || kits.cast || kits.impact) visualKits_.emplace(visualId, kits);
    }
    LOG_INFO("SpellVisual: ", kits_.size(), " kits with effects, ", visualKits_.size(),
             " spell visuals using them (of ", svDbc->getRecordCount(), ")");
}

uint32_t SpellVisualSystem::kitFor(uint32_t visualId, KitKind kind) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto it = visualKits_.find(visualId);
    if (it == visualKits_.end()) return 0;
    switch (kind) {
        case KitKind::Precast: return it->second.precast;
        case KitKind::Cast: return it->second.cast;
        case KitKind::Impact: return it->second.impact;
    }
    return 0;
}

bool SpellVisualSystem::kitEffectTransform(const KitPlacement& at, const SpellKitEffect& effect,
                                           glm::mat4& out) const {
    if (effect.attachmentId == kSpellKitWorldPlaced || at.unitInstanceId == 0) {
        out = glm::scale(glm::translate(glm::mat4(1.0f), at.worldPosition),
                         glm::vec3(spellKitEffectScale(effect, 1.0f)));
        return true;
    }
    auto* characters = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!characters) return false;
    glm::mat4 parent;
    // The attachment, or the unit itself when its model has no such point.
    if (!characters->getAttachmentTransform(at.unitInstanceId, effect.attachmentId, parent) &&
        !characters->getInstanceModelMatrix(at.unitInstanceId, parent)) {
        return false;
    }
    // The size it is drawn at - the unit's own size times the effect's - is
    // held within what the effect allows, so a giant's aura does not fill the
    // zone and a critter's does not vanish.
    const float parentScale = glm::length(glm::vec3(parent[0]));
    if (!(parentScale > 0.0f) || !std::isfinite(parentScale)) return false;
    out = glm::scale(parent, glm::vec3(spellKitEffectScale(effect, parentScale) / parentScale));
    return true;
}

void SpellVisualSystem::playKit(uint32_t kitId, KitKind kind, const KitPlacement& at,
                                uint32_t castTimeMs) {
    const auto kitIt = kits_.find(kitId);
    if (kitIt == kits_.end()) return;
    for (size_t e = 0; e < kitIt->second.effects.size(); ++e) {
        playKitEffect(kitId, e, kind, at, castTimeMs);
    }
}

void SpellVisualSystem::playKitEffect(uint32_t kitId, size_t effectIndex, KitKind kind,
                                      const KitPlacement& at, uint32_t castTimeMs) {
    const SpellKitEffect& effect = kits_.at(kitId).effects[effectIndex];
    const uint32_t modelId = modelIdFor(effect.modelPath);
    if (modelId == 0 || spellVisualFailedModels_.count(modelId)) return;
    // Never loaded in the frame: a model still on its way is waited for, and
    // the effect plays when it lands if it still means anything.
    if (!ensureModel(effect.modelPath, modelId)) {
        deferredPlays_.push_back({.kind = kind, .kitId = kitId, .effectIndex = effectIndex,
                                  .at = at, .castTimeMs = castTimeMs, .modelId = modelId});
        return;
    }
    glm::mat4 transform;
    if (!kitEffectTransform(at, effect, transform)) {
        LOG_WARNING("SpellVisual: kit ", kitId, " effect ", effect.modelPath,
                    " has no unit to hang on (instance ", at.unitInstanceId, "); not shown");
        return;
    }
    // Created where it belongs and then given the whole matrix; an instance
    // made through the scene's changes takes no part in placement dedup.
    auto& models = renderer_->scene().models();
    const uint32_t instanceId = models.createInstance(modelId, glm::vec3(transform[3]),
                                                      glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: could not place kit effect ", effect.modelPath);
        return;
    }
    models.setInstanceTransform(instanceId, transform);

    // As the client plays a kit's model: a precast loops for the cast; a
    // one-shot plays its Stand once, then its Decay if it has one, and stops
    // on its last frame, then finishes (WoW.exe, render spec 6b). How long
    // each lasts is the model's, read before the instance is made.
    float lifetime = 0.0f;
    const float standSeconds = models.animationDuration(instanceId, 0) / 1000.0f;
    if (kind == KitKind::Precast) {
        models.playInstanceAnimation(instanceId, {.animationId = 0, .end = M2AnimationPlay::End::Loop});
        lifetime = castTimeMs > 0 ? static_cast<float>(castTimeMs) / 1000.0f
                                  : std::max(standSeconds, kOneShotFallbackSeconds);
    } else {
        constexpr uint32_t kDecay = 159;
        const bool hasDecay = models.hasAnimation(instanceId, kDecay);
        float decaySeconds = 0.0f;
        if (hasDecay) {
            decaySeconds = models.animationDuration(instanceId, kDecay) / 1000.0f;
            models.playInstanceAnimation(
                instanceId, {.animationId = 0, .end = M2AnimationPlay::End::Next},
                M2AnimationPlay{.animationId = kDecay, .end = M2AnimationPlay::End::Hold});
        } else {
            models.playInstanceAnimation(instanceId, {.animationId = 0, .end = M2AnimationPlay::End::Hold});
        }
        lifetime = standSeconds + decaySeconds;
        // A model whose Stand is a single frame - a burst of particles on a
        // still mesh - still has its particles to show.
        if (lifetime < kOneShotFallbackSeconds) lifetime = kOneShotFallbackSeconds;
    }
    activeEffects_.push_back({.instanceId = instanceId, .kitId = kitId, .effectIndex = effectIndex,
                              .at = at, .lastTransform = transform, .elapsed = 0.0f, .lifetime = lifetime,
                              .isPrecast = kind == KitKind::Precast});
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t castTimeMs, uint32_t attachInstanceId) {
    if (!m2Renderer_ || visualId == 0) return;
    const KitPlacement at{.worldPosition = worldPosition, .unitInstanceId = attachInstanceId};
    if (const uint32_t kit = kitFor(visualId, KitKind::Precast)) {
        playKit(kit, KitKind::Precast, at, castTimeMs);
    }
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit, uint32_t attachInstanceId) {
    if (!m2Renderer_ || visualId == 0) return;
    const KitKind kind = useImpactKit ? KitKind::Impact : KitKind::Cast;
    const KitPlacement at{.worldPosition = worldPosition, .unitInstanceId = attachInstanceId};
    if (const uint32_t kit = kitFor(visualId, kind)) playKit(kit, kind, at, 0);
}

void SpellVisualSystem::playPhysicalProjectile(const std::string& modelPath,
                                                const std::string& texturePath,
                                                const glm::vec3& start,
                                                const glm::vec3& end,
                                                float duration,
                                                bool spin) {
    DeferredProjectile projectile{.modelPath = modelPath, .texturePath = texturePath,
        .start = start, .end = end, .duration = std::max(duration, 0.05f), .spin = spin};
    if (!startProjectile(projectile)) deferredProjectiles_.push_back(std::move(projectile));
}

bool SpellVisualSystem::startProjectile(DeferredProjectile& projectile) {
    if (!renderer_ || projectile.modelPath.empty()) return true;
    auto* characters = renderer_->getCharacterRenderer();
    if (!characters) return true;
    const std::string key = projectile.modelPath + "|" + projectile.texturePath;
    if (failedProjectiles_.contains(key)) return true;
    if (!projectile.texturePath.empty() && !projectile.texture)
        projectile.texture = characters->loadTexture(projectile.texturePath);
    const auto parsed = renderer_->unitModels().request(projectile.modelPath);
    if (parsed.state == pipeline::M2ModelCache::State::Loading || projectile.texture.loading()) return false;
    if (parsed.state == pipeline::M2ModelCache::State::Failed) {
        LOG_WARNING("Physical projectile '", projectile.modelPath, "' could not be loaded; shot has no projectile");
        failedProjectiles_.insert(key);
        return true;
    }
    auto [model, inserted] = projectileModelIds_.try_emplace(key, 0);
    if (inserted) {
        if (nextProjectileModelId_ >= 999000) {
            LOG_WARNING("Physical projectile model IDs exhausted for '", projectile.modelPath,
                        "'; shot has no projectile");
            failedProjectiles_.insert(key);
            return true;
        }
        model->second = nextProjectileModelId_++;
    }
    const uint32_t modelId = model->second;
    if (!characters->getModelData(modelId)) {
        if (!characters->loadModel(parsed.model, modelId)) {
            LOG_WARNING("Physical projectile '", projectile.modelPath,
                        "' GPU preparation failed; shot has no projectile");
            failedProjectiles_.insert(key);
            return true;
        }
        if (projectile.texture) characters->setModelTexture(modelId, 0, projectile.texture);
    }
    const glm::vec3 delta = projectile.end - projectile.start;
    const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    const glm::vec3 rotation(0.0f, -std::atan2(delta.z, std::max(horizontal, 0.001f)),
                             std::atan2(delta.y, delta.x));
    const auto position = glm::mix(projectile.start, projectile.end, projectile.age / projectile.duration);
    const uint32_t instanceId = renderer_->scene().characters().createInstance(modelId, position, rotation, 1.0f);
    if (instanceId != 0) physicalProjectiles_.push_back({.instanceId = instanceId,
        .start = projectile.start, .end = projectile.end, .rotation = rotation, .elapsed = projectile.age,
        .duration = projectile.duration, .spin = projectile.spin});
    return true;
}

void SpellVisualSystem::update(float deltaTime) {
    finishPendingLoads();
    replayDeferredPlays(deltaTime);
    updateStateVisuals(deltaTime);
    if (!preloadPaths_.empty() && m2Renderer_) {
        // A world change clears every M2 model, the character's effects with
        // them; put them back in the background rather than at the next cast.
        preloadCheckTimer_ += deltaTime;
        if (preloadCheckTimer_ >= 2.0f) {
            preloadCheckTimer_ = 0.0f;
            for (const auto& path : preloadPaths_) {
                const auto it = spellVisualModelIds_.find(path);
                if (it != spellVisualModelIds_.end()) ensureModel(path, it->second);
            }
        }
    }
    if (activeEffects_.empty() && physicalProjectiles_.empty() && deferredProjectiles_.empty()) return;

    // Each effect rides its attachment every frame - bone, facing and scale -
    // as the client parents it (0x82e550): a buff landing on a running player
    // runs with them. It used to take the position once, or follow it without
    // turning, and was left standing where it had been cast.
    for (auto it = activeEffects_.begin(); it != activeEffects_.end(); ) {
        it->elapsed += deltaTime;
        bool done = it->elapsed >= it->lifetime;
        if (!done && it->at.unitInstanceId != 0) {
            const SpellKitEffect& effect = kits_.at(it->kitId).effects[it->effectIndex];
            glm::mat4 transform;
            if (!kitEffectTransform(it->at, effect, transform)) {
                done = true;  // the unit is gone
            } else if (transform != it->lastTransform) {
                // Only when the attachment moved: a move refiles the instance
                // in the culling grids.
                renderer_->scene().models().setInstanceTransform(it->instanceId, transform);
                it->lastTransform = transform;
            }
        }
        if (done) {
            renderer_->scene().models().finishInstance(it->instanceId);
            it = activeEffects_.erase(it);
        } else {
            ++it;
        }
    }

    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (charRenderer) {
        auto& characters = renderer_->scene().characters();
        for (auto it = physicalProjectiles_.begin(); it != physicalProjectiles_.end(); ) {
            it->elapsed += deltaTime;
            const float t = std::min(it->elapsed / it->duration, 1.0f);
            characters.setInstancePosition(
                it->instanceId, glm::mix(it->start, it->end, t));
            if (it->spin) {
                glm::vec3 rotation = it->rotation;
                rotation.x += t * glm::two_pi<float>() * 2.0f;
                characters.setInstanceRotation(it->instanceId, rotation);
            }
            if (t >= 1.0f) {
                characters.removeInstance(it->instanceId);
                it = physicalProjectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::erase_if(deferredProjectiles_, [&](DeferredProjectile& projectile) {
        projectile.age += deltaTime;
        if (projectile.age >= projectile.duration) {
            if (expiredProjectiles_.insert(projectile.modelPath).second)
                LOG_WARNING("Physical projectile '", projectile.modelPath,
                            "' was not ready before impact; shot finishes without a projectile");
            return true;
        }
        return startProjectile(projectile);
    });

}

void SpellVisualSystem::cancelAllPrecastVisuals() {
    if (!m2Renderer_ || !renderer_) return;
    // Ended as the client ends an effect: the mesh goes, the particles it has
    // made fly out.
    std::erase_if(activeEffects_, [&](const ActiveEffect& effect) {
        if (!effect.isPrecast) return false;
        renderer_->scene().models().finishInstance(effect.instanceId);
        return true;
    });
    std::erase_if(deferredPlays_, [](const DeferredPlay& play) { return play.kind == KitKind::Precast; });
}

void SpellVisualSystem::reset() {
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    if (renderer_) {
        for (auto& effect : activeEffects_) renderer_->scene().models().removeInstance(effect.instanceId);
        for (const auto& projectile : physicalProjectiles_)
            renderer_->scene().characters().removeInstance(projectile.instanceId);
    }
    activeEffects_.clear();
    physicalProjectiles_.clear();
    deferredProjectiles_.clear();
    failedProjectiles_.clear();
    expiredProjectiles_.clear();
    deferredPlays_.clear();
    resetStateVisuals();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

uint32_t SpellVisualSystem::modelIdFor(const std::string& path) {
    const auto it = spellVisualModelIds_.find(path);
    if (it != spellVisualModelIds_.end()) return it->second;
    if (nextSpellVisualModelId_ >= 999800) {
        LOG_WARNING("SpellVisual: model ID pool exhausted");
        return 0;
    }
    const uint32_t modelId = nextSpellVisualModelId_++;
    spellVisualModelIds_[path] = modelId;
    return modelId;
}

bool SpellVisualSystem::ensureModel(const std::string& path, uint32_t modelId) {
    if (!m2Renderer_ || modelId == 0) return false;
    if (m2Renderer_->hasModel(modelId)) return true;
    if (spellVisualFailedModels_.count(modelId) || pendingModelIds_.count(modelId)) return false;
    if (!cachedAssetManager_) cachedAssetManager_ = renderer_ ? renderer_->getAssetManager() : nullptr;
    auto* am = cachedAssetManager_;
    if (!am) return false;

    PendingLoad load;
    load.path = path;
    load.modelId = modelId;
    load.future = core::ThreadPool::ioWorkers().submit([am, path]() -> PreparedModel {
        PreparedModel out;
        auto data = am->readFile(path);
        if (data.empty()) return out;
        auto model = std::make_shared<pipeline::M2Model>(pipeline::M2Loader::load(data));
        if (model->name.empty()) model->name = path;
        if (model->vertices.empty() && model->particleEmitters.empty()) return out;
        if (model->version >= 264) {
            auto skin = am->readFile(pipeline::skinPathForM2(path));
            if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, *model);
        }
        out.model = std::move(model);
        out.ok = true;
        return out;
    });
    pendingModelIds_.insert(modelId);
    pendingLoads_.push_back(std::move(load));
    return false;
}

void SpellVisualSystem::finishPendingLoads() {
    if (pendingLoads_.empty() || !m2Renderer_) return;
    // Two uploads a frame at most: a login preloading a whole spellbook would
    // otherwise put every one of them on the GPU in the frame they finished.
    int uploaded = 0;
    for (auto it = pendingLoads_.begin(); it != pendingLoads_.end() && uploaded < 2; ) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        PreparedModel prepared = it->future.get();
        const uint32_t modelId = it->modelId;
        const std::string path = it->path;
        pendingModelIds_.erase(modelId);
        it = pendingLoads_.erase(it);
        if (!prepared.ok) {
            LOG_WARNING("SpellVisual: could not load effect model ", path);
            spellVisualFailedModels_.insert(modelId);
            continue;
        }
        const bool loaded = m2Renderer_->loadModel(*prepared.model, modelId);
        ++uploaded;
        if (!loaded) {
            LOG_WARNING("SpellVisual: failed to load effect model to GPU: ", path);
            spellVisualFailedModels_.insert(modelId);
            continue;
        }
        m2Renderer_->markModelAsSpellEffect(modelId);
        // Kept: an effect is cast again and again, and the reaper dropped it a
        // minute after each use, so it was loaded again the next time.
        m2Renderer_->setModelPinned(modelId, true);
        LOG_INFO("SpellVisual: loaded effect model id=", modelId, " path=", path);
    }
}

void SpellVisualSystem::replayDeferredPlays(float deltaTime) {
    if (deferredPlays_.empty() || !m2Renderer_) return;
    std::vector<DeferredPlay> waiting;
    waiting.swap(deferredPlays_);
    for (auto& play : waiting) {
        play.age += deltaTime;
        // How long the effect still means anything: a precast for as long as
        // the cast runs, a cast or impact flash for well under a second.
        const float stillCurrent = play.kind == KitKind::Precast
            ? std::max(0.75f, static_cast<float>(play.castTimeMs) / 1000.0f)
            : 0.75f;
        if (spellVisualFailedModels_.count(play.modelId) || play.age > stillCurrent) continue;
        if (!m2Renderer_->hasModel(play.modelId)) {
            deferredPlays_.push_back(play);
            continue;
        }
        const uint32_t castLeftMs = play.castTimeMs > 0
            ? static_cast<uint32_t>(std::max(0.0f, play.castTimeMs - play.age * 1000.0f)) : 0;
        playKitEffect(play.kitId, play.effectIndex, play.kind, play.at, castLeftMs);
    }
}

void SpellVisualSystem::preloadVisuals(const std::vector<uint32_t>& visualIds) {
    if (!m2Renderer_) return;
    if (!cachedAssetManager_) cachedAssetManager_ = renderer_ ? renderer_->getAssetManager() : nullptr;
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    size_t queued = 0;
    for (const uint32_t visualId : visualIds) {
        for (const KitKind kind : {KitKind::Precast, KitKind::Cast, KitKind::Impact}) {
            const uint32_t kitId = kitFor(visualId, kind);
            if (!kitId) continue;
            for (const auto& effect : kits_.at(kitId).effects) {
                if (!preloadPaths_.insert(effect.modelPath).second) continue;
                const uint32_t modelId = modelIdFor(effect.modelPath);
                if (modelId != 0 && !ensureModel(effect.modelPath, modelId)) ++queued;
            }
        }
    }
    LOG_INFO("SpellVisual: preloading ", queued, " effect model(s) for ", visualIds.size(),
             " spell visual(s); ", preloadPaths_.size(), " kept in all");
}

} // namespace rendering
} // namespace wowee
