// The look a unit keeps while an aura lasts: the StateKit of the aura spell's
// visual. Shadowform is the case that named this - the priest drawn dark
// purple and translucent with a shadow coiling at their feet - but every aura
// with a state kit reads the same columns: Power Word: Shield's bubble, a
// ghost's blue, Ice Block's frost.
//
// The game side says, a few times a second, which units carry which visuals
// (syncStateVisuals); this file turns that into effect models hung on the
// units and a colour and opacity for their bodies, and keeps the models on
// them as they move.

#include "rendering/spell_visual_system.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace wowee {
namespace rendering {

namespace {

constexpr uint32_t kNoField = 0xFFFFFFFFu;

// SpellVisualKit character proc types (wowdev.wiki DB/SpellVisualKit).
constexpr uint32_t kCharProcColor = 1;
constexpr uint32_t kCharProcAlpha = 14;

// How long a body takes to go into an aura's look and back out of it.
constexpr float kStateLookFadeSeconds = 0.5f;

float stepTowards(float value, float target, float step) {
    if (value < target) return std::min(value + step, target);
    return std::max(value - step, target);
}

} // namespace

void SpellVisualSystem::loadStateKits() {
    if (stateKitsLoaded_) return;
    if (!cachedAssetManager_) cachedAssetManager_ = renderer_ ? renderer_->getAssetManager() : nullptr;
    if (!cachedAssetManager_) return;  // assets not up yet; asked again next time
    stateKitsLoaded_ = true;

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;
    // Asked, not demanded: only the WotLK layout is known to carry these
    // columns. An expansion without them simply has no aura looks.
    const uint32_t stateKitField = svLayout ? svLayout->tryField("StateKit") : kNoField;
    if (stateKitField == kNoField || !kitLayout || !fxLayout) {
        LOG_WARNING("SpellVisual: no StateKit column in this expansion's layout (",
                    layout ? "the layout has none" : "no layout loaded",
                    "); aura looks - Shadowform's, a ghost's - are not drawn");
        return;
    }

    auto svDbc  = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    if (!svDbc || !svDbc->isLoaded() || !kitDbc || !kitDbc->isLoaded()) {
        LOG_WARNING("SpellVisual: state kits not loaded - a spell visual DBC is missing");
        return;
    }
    // The kits' effect models, read once for every kind of kit.
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // SpellVisualKit: id -> what the kit hangs on the unit and does to it.
    const uint32_t kitFc = kitDbc->getFieldCount();
    const uint32_t procField  = kitLayout->tryField("CharProc0");
    const uint32_t paramZero  = kitLayout->tryField("CharParamZero0");
    const bool hasProcs = procField != kNoField && paramZero != kNoField &&
                          procField + 4 <= kitFc && paramZero + 4 <= kitFc;
    const uint32_t flagsField = kitLayout->field("Flags");

    std::unordered_map<uint32_t, StateKit> kits;
    for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
        const uint32_t kitId = kitDbc->getUInt32(i, 0);
        if (!kitId) continue;
        StateKit kit;
        if (const auto effectsIt = kits_.find(kitId); effectsIt != kits_.end())
            kit.effects = effectsIt->second.effects;
        constexpr uint32_t kKitLoopsStand = 0x20;
        kit.loopStand = flagsField < kitFc && (kitDbc->getUInt32(i, flagsField) & kKitLoopsStand) != 0;
        if (hasProcs) {
            for (uint32_t p = 0; p < 4; ++p) {
                const uint32_t type = kitDbc->getUInt32(i, procField + p);
                const float param = kitDbc->getFloat(i, paramZero + p);
                if (!std::isfinite(param)) continue;
                if (type == kCharProcColor) {
                    // The colour is stored as a float whose value is 0xRRGGBB:
                    // Shadowform's 2555970 is 0x270042, a deep purple.
                    const auto rgb = static_cast<uint32_t>(std::clamp(param, 0.0f, 16777215.0f));
                    kit.tint = glm::vec3(static_cast<float>((rgb >> 16) & 0xFF),
                                         static_cast<float>((rgb >> 8) & 0xFF),
                                         static_cast<float>(rgb & 0xFF)) / 255.0f;
                } else if (type == kCharProcAlpha) {
                    kit.alpha = std::clamp(param, 0.0f, 1.0f);
                }
            }
        }
        if (kit.effects.empty() && kit.tint == glm::vec3(1.0f) && kit.alpha >= 1.0f) continue;
        kits.emplace(kitId, std::move(kit));
    }

    // SpellVisual: visual -> its state kit.
    const uint32_t svFc = svDbc->getFieldCount();
    if (stateKitField >= svFc) return;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        const uint32_t visualId = svDbc->getUInt32(i, 0);
        const auto it = kits.find(svDbc->getUInt32(i, stateKitField));
        if (visualId && it != kits.end()) stateKits_[visualId] = it->second;
    }
    LOG_INFO("SpellVisual: ", stateKits_.size(), " spell visuals have a state kit (aura look)");
}

void SpellVisualSystem::syncStateVisuals(const std::vector<UnitStateVisuals>& units) {
    if (!m2Renderer_ || !renderer_) return;
    if (!stateKitsLoaded_) loadStateKits();
    auto* charRenderer = renderer_->getCharacterRenderer();

    std::unordered_set<uint64_t> present;
    present.reserve(units.size());
    for (const auto& unit : units) {
        if (unit.guid == 0 || unit.instanceId == 0) continue;
        std::vector<uint32_t> visualIds;
        for (const uint32_t visualId : unit.visualIds) {
            if (stateKits_.count(visualId)) visualIds.push_back(visualId);
        }
        std::sort(visualIds.begin(), visualIds.end());
        visualIds.erase(std::unique(visualIds.begin(), visualIds.end()), visualIds.end());

        auto it = unitStates_.find(unit.guid);
        if (it == unitStates_.end()) {
            if (visualIds.empty()) continue;
            it = unitStates_.emplace(unit.guid, UnitState{}).first;
            it->second.instanceId = unit.instanceId;
        }
        present.insert(unit.guid);
        UnitState& state = it->second;
        if (state.instanceId != unit.instanceId) {
            // Drawn again - a respawn, a new model. The old body is gone or
            // going; the new one starts from its own look.
            removeStateEffects(state);
            if (charRenderer) charRenderer->setInstanceStateLook(state.instanceId, glm::vec3(1.0f), 1.0f);
            state = UnitState{};
            state.instanceId = unit.instanceId;
        }
        setUnitStateKits(state, std::move(visualIds));
    }
    // Out of sight, or no auras left: take the look off. The entry goes once
    // the body has faded back.
    for (auto& [guid, state] : unitStates_) {
        if (!present.count(guid)) setUnitStateKits(state, {});
    }
}

void SpellVisualSystem::setUnitStateKits(UnitState& state, std::vector<uint32_t> visualIds) {
    if (state.visualIds == visualIds) return;

    // Effects of visuals that are gone come down; the rest stay as they are,
    // so one buff landing does not restart every other effect on the unit.
    for (auto it = state.effects.begin(); it != state.effects.end(); ) {
        if (std::binary_search(visualIds.begin(), visualIds.end(), it->visualId)) {
            ++it;
            continue;
        }
        // As the client ends it: the mesh goes at once, its particles fly out.
        if (it->instanceId != 0 && m2Renderer_) m2Renderer_->finishInstance(it->instanceId);
        it = state.effects.erase(it);
    }
    glm::vec3 tint(1.0f);
    float alpha = 1.0f;
    for (const uint32_t visualId : visualIds) {
        const auto kitIt = stateKits_.find(visualId);
        if (kitIt == stateKits_.end()) continue;
        const StateKit& kit = kitIt->second;
        tint *= kit.tint;
        alpha = std::min(alpha, kit.alpha);
        if (std::binary_search(state.visualIds.begin(), state.visualIds.end(), visualId)) continue;
        for (size_t e = 0; e < kit.effects.size(); ++e) {
            const uint32_t modelId = modelIdFor(kit.effects[e].modelPath);
            if (modelId == 0) continue;
            // Instanced in update, once the model is resident.
            state.effects.push_back({.visualId = visualId, .effectIndex = e, .modelId = modelId});
        }
    }
    state.targetTint = tint;
    state.targetAlpha = alpha;
    state.visualIds = std::move(visualIds);
}

void SpellVisualSystem::removeStateEffects(UnitState& state) {
    if (m2Renderer_) {
        for (const auto& effect : state.effects) {
            if (effect.instanceId != 0) m2Renderer_->finishInstance(effect.instanceId);
        }
    }
    state.effects.clear();
}

void SpellVisualSystem::updateStateVisuals(float deltaTime) {
    if (unitStates_.empty() || !m2Renderer_) return;
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const float step = deltaTime / kStateLookFadeSeconds;

    for (auto it = unitStates_.begin(); it != unitStates_.end(); ) {
        UnitState& state = it->second;
        for (auto& effect : state.effects) {
            const auto kitIt = stateKits_.find(effect.visualId);
            if (kitIt == stateKits_.end() || effect.effectIndex >= kitIt->second.effects.size()) continue;
            const SpellKitEffect& kitEffect = kitIt->second.effects[effect.effectIndex];
            // Hung on the unit - the attachment's whole matrix, so it turns
            // and sways with the body. A World-column effect stays where the
            // unit stood when it appeared.
            if (effect.instanceId == 0 && charRenderer) {
                glm::mat4 unitMatrix;
                if (charRenderer->getInstanceModelMatrix(state.instanceId, unitMatrix))
                    effect.worldPosition = glm::vec3(unitMatrix[3]);
            }
            glm::mat4 transform;
            if (!kitEffectTransform({.worldPosition = effect.worldPosition, .unitInstanceId = state.instanceId},
                                    kitEffect, transform)) {
                continue;
            }
            if (effect.instanceId == 0) {
                if (spellVisualFailedModels_.count(effect.modelId)) continue;
                if (!ensureModel(kitEffect.modelPath, effect.modelId)) continue;  // still loading
                // Created where it belongs and then given the whole matrix;
                // a child instance takes no part in placement dedup.
                effect.instanceId = m2Renderer_->createInstance(effect.modelId, glm::vec3(transform[3]),
                                                                glm::vec3(0.0f), 1.0f,
                                                                /*allowPositionDedup=*/false);
                if (effect.instanceId == 0) {
                    LOG_WARNING("SpellVisual: aura effect '", kitEffect.modelPath, "' (visual ",
                                effect.visualId, ") could not be placed on its unit; not drawn");
                    spellVisualFailedModels_.insert(effect.modelId);
                    continue;
                }
                m2Renderer_->setInstanceTransform(effect.instanceId, transform);
                effect.lastTransform = transform;
                // As the client plays an aura's model (render spec 6b): its
                // Stand once, then its Hold over and over for as long as the
                // aura lasts - or the Stand on a loop where the kit says so
                // (flag 0x20) or the model has no Hold. Mana Shield appears
                // in its Stand and pulses gently in its Hold; looping the
                // Stand, it appeared again and again.
                constexpr uint32_t kHold = 158;
                using End = M2AnimationPlay::End;
                if (!kitIt->second.loopStand && m2Renderer_->hasAnimation(effect.instanceId, kHold)) {
                    m2Renderer_->playInstanceAnimation(effect.instanceId, {.animationId = 0, .end = End::Next},
                                                       M2AnimationPlay{.animationId = kHold, .end = End::Loop});
                } else {
                    m2Renderer_->playInstanceAnimation(effect.instanceId, {.animationId = 0, .end = End::Loop});
                }
            } else if (transform != effect.lastTransform) {
                // Only when the attachment moved: a move refiles the instance
                // in the culling grids.
                m2Renderer_->setInstanceTransform(effect.instanceId, transform);
                effect.lastTransform = transform;
            }
            // The effect is as see-through as the body it hangs on (WoW.exe
            // 0x82f27f): it fades in and out with its unit, a stealthed one
            // or a ghost.
            float unitAlpha = 1.0f;
            if (charRenderer) charRenderer->getInstanceDrawnOpacity(state.instanceId, unitAlpha);
            if (unitAlpha != effect.lastAlpha) {
                m2Renderer_->setInstanceFade(effect.instanceId, unitAlpha);
                effect.lastAlpha = unitAlpha;
            }
        }

        if (state.tint != state.targetTint || state.alpha != state.targetAlpha) {
            state.tint = glm::vec3(stepTowards(state.tint.r, state.targetTint.r, step),
                                   stepTowards(state.tint.g, state.targetTint.g, step),
                                   stepTowards(state.tint.b, state.targetTint.b, step));
            state.alpha = stepTowards(state.alpha, state.targetAlpha, step);
            if (charRenderer) charRenderer->setInstanceStateLook(state.instanceId, state.tint, state.alpha);
        }

        // Nothing left on it and its own look back: forget the unit.
        if (state.visualIds.empty() && state.effects.empty() &&
            state.tint == glm::vec3(1.0f) && state.alpha >= 1.0f) {
            it = unitStates_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::resetStateVisuals() {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    for (auto& [guid, state] : unitStates_) {
        removeStateEffects(state);
        if (charRenderer) charRenderer->setInstanceStateLook(state.instanceId, glm::vec3(1.0f), 1.0f);
    }
    unitStates_.clear();
}

} // namespace rendering
} // namespace wowee
