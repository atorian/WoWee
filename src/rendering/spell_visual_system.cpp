#include "rendering/spell_visual_system.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

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

// Load SpellVisual DBC chain: SpellVisualEffectName → SpellVisualKit → SpellVisual
// to build cast/impact M2 path lookup maps.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;

    if (!cachedAssetManager_) {
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    }
    // Not an attempt. "Set early to prevent re-entry on failure" was the
    // intent, but there is no failure yet - only assets that have not
    // arrived. Latching here left spell visuals off for the session
    // whenever this was reached first.
    if (!cachedAssetManager_) return;
    spellVisualDbcLoaded_ = true;  // a real attempt follows; do not repeat it

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;

    uint32_t svCastKitField   = svLayout  ? (*svLayout)["CastKit"]       : 2;
    uint32_t svPrecastKitField = svLayout  ? (*svLayout)["PrecastKit"]    : 1;
    uint32_t svImpactKitField = svLayout  ? (*svLayout)["ImpactKit"]     : 3;
    uint32_t svMissileField   = svLayout  ? (*svLayout)["MissileModel"]  : 8;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;

    // Kit effect fields to probe, in priority order.
    // SpecialEffect0 > BaseEffect > LeftHand > RightHand > Chest > Head > Breath
    struct KitField { const char* name; uint32_t fallback; };
    static constexpr KitField kitFieldDefs[] = {
        {.name = "SpecialEffect0",  .fallback = 11}, {.name = "BaseEffect",       .fallback = 5},
        {.name = "LeftHandEffect",   .fallback = 6}, {.name = "RightHandEffect",  .fallback = 7},
        {.name = "ChestEffect",      .fallback = 4}, {.name = "HeadEffect",       .fallback = 3},
        {.name = "BreathEffect",     .fallback = 8}, {.name = "SpecialEffect1",  .fallback = 12},
        {.name = "SpecialEffect2",  .fallback = 13},
    };
    constexpr size_t numKitFields = sizeof(kitFieldDefs) / sizeof(kitFieldDefs[0]);
    uint32_t kitFields[numKitFields];
    for (size_t k = 0; k < numKitFields; ++k)
        kitFields[k] = kitLayout ? kitLayout->field(kitFieldDefs[k].name) : kitFieldDefs[k].fallback;

    // Load SpellVisualEffectName.dbc - ID → M2 path
    auto fxDbc = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!fxDbc || !fxDbc->isLoaded() || fxDbc->getFieldCount() <= fxFilePathField) {
        LOG_DEBUG("SpellVisual: SpellVisualEffectName.dbc unavailable (fc=",
                  fxDbc ? fxDbc->getFieldCount() : 0, ")");
        return;
    }
    std::unordered_map<uint32_t, std::string> effectPaths; // effectNameId → path
    for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
        uint32_t id   = fxDbc->getUInt32(i, 0);
        std::string p = fxDbc->getString(i, fxFilePathField);
        if (id && !p.empty()) {
            // The DBC stores the extension the art was authored with; what
            // shipped is .m2.
            p = pipeline::modelPathToM2(p);
            effectPaths[id] = p;
        }
    }

    // Load SpellVisualKit.dbc - kitId → best SpellVisualEffectName ID
    // Probes all effect slots in priority order and keeps the first valid hit.
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    std::unordered_map<uint32_t, uint32_t> kitToEffectName; // kitId → effectNameId
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            uint32_t eff = 0;
            for (size_t k = 0; k < numKitFields && !eff; ++k) {
                if (kitFields[k] < fc)
                    eff = kitDbc->getUInt32(i, kitFields[k]);
            }
            if (eff) kitToEffectName[kitId] = eff;
        }
    }

    // Helper: resolve path for a given kit ID
    auto kitPath = [&](uint32_t kitId) -> std::string {
        if (!kitId) return {};
        auto kitIt = kitToEffectName.find(kitId);
        if (kitIt == kitToEffectName.end()) return {};
        auto fxIt = effectPaths.find(kitIt->second);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };
    auto missilePath = [&](uint32_t effId) -> std::string {
        if (!effId) return {};
        auto fxIt = effectPaths.find(effId);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };

    // Load SpellVisual.dbc - visualId → cast/impact M2 paths via kit chain
    auto svDbc = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    if (!svDbc || !svDbc->isLoaded()) {
        LOG_DEBUG("SpellVisual: SpellVisual.dbc unavailable");
        return;
    }
    uint32_t svFc = svDbc->getFieldCount();
    uint32_t loadedPrecast = 0, loadedCast = 0, loadedImpact = 0;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        uint32_t vid = svDbc->getUInt32(i, 0);
        if (!vid) continue;

        // Precast path: PrecastKit → SpecialEffect0/BaseEffect
        {
            std::string path;
            if (svPrecastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svPrecastKitField));
            if (!path.empty()) { spellVisualPrecastPath_[vid] = path; ++loadedPrecast; }
        }
        // Cast path: CastKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        {
            std::string path;
            if (svCastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svCastKitField));
            if (path.empty() && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualCastPath_[vid] = path; ++loadedCast; }
        }
        // Impact path: ImpactKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        {
            std::string path;
            if (svImpactKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svImpactKitField));
            if (path.empty() && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualImpactPath_[vid] = path; ++loadedImpact; }
        }
    }
    LOG_INFO("SpellVisual: loaded precast=", loadedPrecast, " cast=", loadedCast, " impact=", loadedImpact,
             " visual\u2192M2 mappings (of ", svDbc->getRecordCount(), " records)");
}

// ---------------------------------------------------------------------------
// Classify model path to a character attachment point for bone tracking
// ---------------------------------------------------------------------------
uint32_t SpellVisualSystem::classifyAttachmentId(const std::string& modelPath) {
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects track the right hand (attachment 1)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos)
        return 1; // RightHand
    // "chest" effects track chest/torso (attachment 5 in M2 spec)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos)
        return 5; // Chest
    // "head" effects track head (attachment 11)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos)
        return 11; // Head
    return 0; // No bone tracking (static position or base effect)
}

// ---------------------------------------------------------------------------
// Height offset for spell effect placement (fallback when no bone tracking)
// ---------------------------------------------------------------------------
glm::vec3 SpellVisualSystem::applyEffectHeightOffset(const glm::vec3& basePos, const std::string& modelPath) {
    // Lowercase the path for case-insensitive matching
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects go at hand height (~0.8m above feet)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 0.8f);
    }
    // "chest" effects go at chest height (~1.0m above feet)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.0f);
    }
    // "head" effects go at head height (~1.6m above feet)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.6f);
    }
    // "base" / "feet" / ground effects stay at ground level
    return basePos;
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t castTimeMs, uint32_t attachInstanceId) {
    LOG_INFO("SpellVisual: playSpellVisualPrecast visualId=", visualId,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z,
             ") castTimeMs=", castTimeMs);
    if (!m2Renderer_ || visualId == 0) {
        LOG_WARNING("SpellVisual: playSpellVisualPrecast early-out: m2Renderer_=", (m2Renderer_ ? "yes" : "null"),
                    " visualId=", visualId);
        return;
    }

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) { LOG_WARNING("SpellVisual: no AssetManager"); return; }

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // Try precast path first, fall back to cast path
    auto pathIt = spellVisualPrecastPath_.find(visualId);
    if (pathIt == spellVisualPrecastPath_.end()) {
        // No precast kit - fall back to playing cast kit
        playSpellVisual(visualId, worldPosition, false, attachInstanceId);
        return;
    }

    const std::string& modelPath = pathIt->second;
    LOG_INFO("SpellVisual: precast path resolved to: ", modelPath);

    const uint32_t modelId = modelIdFor(modelPath);
    if (modelId == 0) return;
    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: precast model in failed-cache, skipping: ", modelPath);
        return;
    }
    // Never loaded in the frame: a model still on its way is waited for, and
    // the effect plays when it lands if the cast is still going.
    if (!ensureModel(modelPath, modelId)) {
        deferredPlays_.push_back({.kind = DeferredPlay::Kind::Precast, .visualId = visualId,
                                  .position = worldPosition, .castTimeMs = castTimeMs,
                                  .attachInstanceId = attachInstanceId, .modelId = modelId});
        return;
    }

    // Determine attachment point for bone tracking (hand/chest/head → follow
    // the CASTER's bones; attachInstanceId=0 means a non-tracked caster, so
    // the effect stays static at their world position).
    uint32_t attachId = classifyAttachmentId(modelPath);
    if (attachInstanceId == 0) attachId = 0;
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        if (charRenderer) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(attachInstanceId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: createInstance returned 0 for precast model=", modelPath);
        return;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);

    // Duration: prefer server cast time if available (long casts like Hearthstone=10s),
    // otherwise fall back to M2 animation duration, then default.
    float duration;
    if (castTimeMs >= 500) {
        // Server cast time available - precast should last the full cast duration
        duration = std::clamp(static_cast<float>(castTimeMs) / 1000.0f, 0.5f, 30.0f);
    } else {
        float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
        duration = (animDurMs > 100.0f)
            ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
            : SPELL_VISUAL_DEFAULT_DURATION;
    }
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration, .isPrecast = true, .attachmentId = attachId, .attachInstanceId = attachInstanceId});
    LOG_INFO("SpellVisual: spawned precast visualId=", visualId, " instanceId=", instanceId,
             " duration=", duration, "s castTimeMs=", castTimeMs, " attach=", attachId,
             " model=", modelPath,
             " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the caster's left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            if (cr) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(attachInstanceId, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            m2Renderer_->restartInstanceAnimation(leftId);
            activeSpellVisuals_.push_back({.instanceId = leftId, .elapsed = 0.0f, .duration = duration, .isPrecast = true, .attachmentId = 2 /* LeftHand */, .attachInstanceId = attachInstanceId});
        }
    }
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit, uint32_t attachInstanceId) {
    LOG_INFO("SpellVisual: playSpellVisual visualId=", visualId, " impact=", useImpactKit,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z, ")");
    if (!m2Renderer_ || visualId == 0) return;

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // Select cast or impact path map; fall back to the other if missing
    auto& primaryMap = useImpactKit ? spellVisualImpactPath_ : spellVisualCastPath_;
    auto& fallbackMap = useImpactKit ? spellVisualCastPath_ : spellVisualImpactPath_;
    auto pathIt = primaryMap.find(visualId);
    if (pathIt == primaryMap.end()) {
        pathIt = fallbackMap.find(visualId);
        if (pathIt == fallbackMap.end()) {
            return;
        }
    }

    const std::string& modelPath = pathIt->second;
    LOG_INFO("SpellVisual: ", (useImpactKit ? "impact" : "cast"), " path resolved to: ", modelPath);

    const uint32_t modelId = modelIdFor(modelPath);
    if (modelId == 0) return;
    // Skip models that have previously failed to load (avoid repeated I/O)
    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: model in failed-cache, skipping: ", modelPath);
        return;
    }
    if (!ensureModel(modelPath, modelId)) {
        deferredPlays_.push_back({.kind = useImpactKit ? DeferredPlay::Kind::Impact
                                                       : DeferredPlay::Kind::Cast,
                                  .visualId = visualId, .position = worldPosition,
                                  .attachInstanceId = attachInstanceId, .modelId = modelId});
        return;
    }

    // Determine attachment point for bone tracking on cast effects. Only the
    // caster identified by attachInstanceId may be tracked - never default to
    // the local player (that glued every nearby unit's cast kit to the
    // player's hands).
    uint32_t attachId = 0;
    if (!useImpactKit && attachInstanceId != 0) {
        attachId = classifyAttachmentId(modelPath);
    }
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        if (charRenderer) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(attachInstanceId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    // Spawn instance at world position
    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: failed to create instance for visualId=", visualId);
        return;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);
    // Determine lifetime from M2 animation duration (clamp to reasonable range)
    float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    float duration = (animDurMs > 100.0f)
        ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
        : SPELL_VISUAL_DEFAULT_DURATION;
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration, .isPrecast = false, .attachmentId = attachId, .attachInstanceId = attachInstanceId});
    LOG_INFO("SpellVisual: spawned ", (useImpactKit ? "impact" : "cast"), " visualId=", visualId,
             " instanceId=", instanceId, " duration=", duration, "s animDurMs=", animDurMs,
             " attach=", attachId, " model=", modelPath, " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the caster's left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            if (cr) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(attachInstanceId, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            m2Renderer_->restartInstanceAnimation(leftId);
            activeSpellVisuals_.push_back({.instanceId = leftId, .elapsed = 0.0f, .duration = duration, .isPrecast = false, .attachmentId = 2 /* LeftHand */, .attachInstanceId = attachInstanceId});
        }
    }
}

void SpellVisualSystem::playPhysicalProjectile(const std::string& modelPath,
                                                const std::string& texturePath,
                                                const glm::vec3& start,
                                                const glm::vec3& end,
                                                float duration,
                                                bool spin) {
    if (!renderer_ || modelPath.empty()) return;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    if (!characterRenderer) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    const std::string cacheKey = modelPath + "|" + texturePath;
    uint32_t modelId = 0;
    auto cached = projectileModelIds_.find(cacheKey);
    if (cached != projectileModelIds_.end()) {
        modelId = cached->second;
    } else {
        if (nextProjectileModelId_ >= 999000) return;
        modelId = nextProjectileModelId_++;
        projectileModelIds_[cacheKey] = modelId;
    }

    if (!characterRenderer->getModelData(modelId)) {
        auto bytes = cachedAssetManager_->readFile(modelPath);
        if (bytes.empty()) return;
        pipeline::M2Model model = pipeline::M2Loader::load(bytes);
        if (model.name.empty()) model.name = modelPath;
        if (model.version >= 264) {
            const std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skin = cachedAssetManager_->readFile(skinPath);
            if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
        }
        if (model.vertices.empty() || !characterRenderer->loadModel(model, modelId)) return;
        if (!texturePath.empty()) {
            if (auto* texture = characterRenderer->loadTexture(texturePath))
                characterRenderer->setModelTexture(modelId, 0, texture);
        }
    }

    const glm::vec3 delta = end - start;
    const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    glm::vec3 rotation(0.0f);
    rotation.z = std::atan2(delta.y, delta.x);
    rotation.y = -std::atan2(delta.z, std::max(horizontal, 0.001f));

    const uint32_t instanceId = characterRenderer->createInstance(modelId, start, rotation, 1.0f);
    if (instanceId == 0) return;
    physicalProjectiles_.push_back({.instanceId = instanceId, .start = start, .end = end, .rotation = rotation, .elapsed = 0.0f,
                                    .duration = std::max(duration, 0.05f), .spin = spin});
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
    if (activeSpellVisuals_.empty() && physicalProjectiles_.empty()) return;

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;

    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        it->elapsed += deltaTime;
        if (it->elapsed >= it->duration) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            // Update position for bone-tracked effects - follow the CASTER's
            // hands/chest/head, not the local player's.
            if (it->attachmentId != 0 && it->attachInstanceId != 0 && charRenderer) {
                glm::mat4 attachMat;
                if (charRenderer->getAttachmentTransform(it->attachInstanceId, it->attachmentId, attachMat)) {
                    glm::vec3 bonePos = glm::vec3(attachMat[3]);
                    m2Renderer_->setInstancePosition(it->instanceId, bonePos);
                }
            }
            ++it;
        }
    }


    if (charRenderer) {
        for (auto it = physicalProjectiles_.begin(); it != physicalProjectiles_.end(); ) {
            it->elapsed += deltaTime;
            const float t = std::min(it->elapsed / it->duration, 1.0f);
            charRenderer->setInstancePosition(
                it->instanceId, glm::mix(it->start, it->end, t));
            if (it->spin) {
                glm::vec3 rotation = it->rotation;
                rotation.x += t * glm::two_pi<float>() * 2.0f;
                charRenderer->setInstanceRotation(it->instanceId, rotation);
            }
            if (t >= 1.0f) {
                charRenderer->removeInstance(it->instanceId);
                it = physicalProjectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void SpellVisualSystem::cancelAllPrecastVisuals() {
    if (!m2Renderer_) return;
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->isPrecast) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::reset() {
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) {
        if (m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    }
    activeSpellVisuals_.clear();
    if (renderer_ && renderer_->getCharacterRenderer()) {
        for (const auto& projectile : physicalProjectiles_)
            renderer_->getCharacterRenderer()->removeInstance(projectile.instanceId);
    }
    physicalProjectiles_.clear();
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
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
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
        // Decoded here, keyed as M2Renderer::loadModel asks for them, so the
        // upload on the main thread reads nothing from disk.
        for (const auto& tex : model->textures) {
            std::string key = tex.filename;
            const size_t nul = key.find('\0');
            if (nul != std::string::npos) key.resize(nul);
            if (key.empty()) continue;
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (out.textures.count(key)) continue;
            auto blp = am->loadTexture(key, true);
            if (blp.isValid()) out.textures[key] = std::move(blp);
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
        m2Renderer_->setPredecodedBLPCache(&prepared.textures);
        const bool loaded = m2Renderer_->loadModel(*prepared.model, modelId);
        m2Renderer_->setPredecodedBLPCache(nullptr);
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
        const float stillCurrent = play.kind == DeferredPlay::Kind::Precast
            ? std::max(0.75f, static_cast<float>(play.castTimeMs) / 1000.0f)
            : 0.75f;
        if (spellVisualFailedModels_.count(play.modelId) || play.age > stillCurrent) continue;
        if (!m2Renderer_->hasModel(play.modelId)) {
            deferredPlays_.push_back(play);
            continue;
        }
        switch (play.kind) {
            case DeferredPlay::Kind::Precast:
                playSpellVisualPrecast(play.visualId, play.position,
                                       play.castTimeMs > 0 ? static_cast<uint32_t>(
                                           std::max(0.0f, play.castTimeMs - play.age * 1000.0f))
                                                           : 0,
                                       play.attachInstanceId);
                break;
            case DeferredPlay::Kind::Cast:
                playSpellVisual(play.visualId, play.position, false, play.attachInstanceId);
                break;
            case DeferredPlay::Kind::Impact:
                playSpellVisual(play.visualId, play.position, true, play.attachInstanceId);
                break;
        }
    }
}

void SpellVisualSystem::preloadVisuals(const std::vector<uint32_t>& visualIds) {
    if (!m2Renderer_) return;
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    size_t queued = 0;
    for (const uint32_t visualId : visualIds) {
        for (const auto* map : {&spellVisualPrecastPath_, &spellVisualCastPath_, &spellVisualImpactPath_}) {
            const auto it = map->find(visualId);
            if (it == map->end() || it->second.empty()) continue;
            if (!preloadPaths_.insert(it->second).second) continue;
            const uint32_t modelId = modelIdFor(it->second);
            if (modelId != 0 && !ensureModel(it->second, modelId)) ++queued;
        }
    }
    LOG_INFO("SpellVisual: preloading ", queued, " effect model(s) for ", visualIds.size(),
             " spell visual(s); ", preloadPaths_.size(), " kept in all");
}

} // namespace rendering
} // namespace wowee
