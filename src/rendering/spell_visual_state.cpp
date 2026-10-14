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

// The M2 attachment each SpellVisualKit effect column hangs from, as the
// client's kit play (WoW.exe 3.3.5a 12340, 0x745230) has them: the head effect
// at Head, not Helm; the chest effect at Chest, not ChestBloodFront; the base
// at Base - a state kit's base too; the specials at Special1-3. The client
// parents the effect to the attachment (CM2Model::AttachToParent 0x831630) and
// it follows the attachment's whole matrix from then on. A model without the
// attachment carries the effect at its own origin.
struct KitEffectColumn {
    const char* name;
    uint32_t attachmentId;
};
constexpr KitEffectColumn kKitEffectAttachments[] = {
    {.name = "HeadEffect",      .attachmentId = 20},  // Head
    {.name = "ChestEffect",     .attachmentId = 34},  // Chest
    {.name = "BaseEffect",      .attachmentId = 19},  // Base
    {.name = "LeftHandEffect",  .attachmentId = 21},  // SpellLeftHand
    {.name = "RightHandEffect", .attachmentId = 22},  // SpellRightHand
    {.name = "BreathEffect",    .attachmentId = 17},  // Breath
    {.name = "SpecialEffect0",  .attachmentId = 23},  // Special1
    {.name = "SpecialEffect1",  .attachmentId = 24},  // Special2
    {.name = "SpecialEffect2",  .attachmentId = 25},  // Special3
};

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
    auto fxDbc  = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!svDbc || !svDbc->isLoaded() || !kitDbc || !kitDbc->isLoaded() ||
        !fxDbc || !fxDbc->isLoaded()) {
        LOG_WARNING("SpellVisual: state kits not loaded - a spell visual DBC is missing");
        return;
    }

    // SpellVisualEffectName: id -> model, its scale and the sizes allowed it.
    std::unordered_map<uint32_t, StateKit::Effect> effectNames;
    {
        const uint32_t pathField  = fxLayout->tryField("FilePath");
        const uint32_t scaleField = fxLayout->tryField("Scale");
        const uint32_t minField   = fxLayout->tryField("MinAllowedScale");
        const uint32_t maxField   = fxLayout->tryField("MaxAllowedScale");
        const uint32_t fc = fxDbc->getFieldCount();
        if (pathField >= fc) return;
        const auto positive = [&](uint32_t row, uint32_t field, float fallback) {
            if (field >= fc) return fallback;
            const float v = fxDbc->getFloat(row, field);
            return std::isfinite(v) && v > 0.0f ? v : fallback;
        };
        for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
            const uint32_t id = fxDbc->getUInt32(i, 0);
            std::string path = fxDbc->getString(i, pathField);
            if (!id || path.empty()) continue;
            StateKit::Effect effect{.path = pipeline::modelPathToM2(path),
                                    .scale = positive(i, scaleField, 1.0f)};
            effect.minScale = positive(i, minField, 0.0f);
            effect.maxScale = positive(i, maxField, std::numeric_limits<float>::max());
            if (effect.maxScale < effect.minScale) std::swap(effect.minScale, effect.maxScale);
            effectNames[id] = std::move(effect);
        }
    }

    // SpellVisualKit: id -> what the kit hangs on the unit and does to it.
    const uint32_t kitFc = kitDbc->getFieldCount();
    const uint32_t procField  = kitLayout->tryField("CharProc0");
    const uint32_t paramZero  = kitLayout->tryField("CharParamZero0");
    const bool hasProcs = procField != kNoField && paramZero != kNoField &&
                          procField + 4 <= kitFc && paramZero + 4 <= kitFc;

    std::unordered_map<uint32_t, StateKit> kits;
    for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
        const uint32_t kitId = kitDbc->getUInt32(i, 0);
        if (!kitId) continue;
        StateKit kit;
        for (const auto& column : kKitEffectAttachments) {
            const uint32_t field = kitLayout->tryField(column.name);
            if (field >= kitFc) continue;
            const uint32_t effectId = kitDbc->getUInt32(i, field);
            const auto it = effectNames.find(effectId);
            if (it == effectNames.end()) continue;
            StateKit::Effect effect = it->second;
            effect.attachmentId = column.attachmentId;
            kit.effects.push_back(std::move(effect));
        }
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
        if (it->instanceId != 0 && m2Renderer_) m2Renderer_->removeInstance(it->instanceId);
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
            const uint32_t modelId = modelIdFor(kit.effects[e].path);
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
            if (effect.instanceId != 0) m2Renderer_->removeInstance(effect.instanceId);
        }
    }
    state.effects.clear();
}

bool SpellVisualSystem::stateEffectTransform(uint32_t charInstanceId,
                                             const StateKit::Effect& effect, glm::mat4& out) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer) return false;
    // The attachment's own matrix, bone and all: the effect turns with the
    // unit and sways with the body. A tournament pennant is modelled leaning
    // back from the chest, so placed by position alone it pointed the same
    // way in the world whichever way its bearer faced. A model without the
    // attachment carries the effect at its origin, as the client does.
    glm::mat4 anchor;
    if (!charRenderer->getAttachmentTransform(charInstanceId, effect.attachmentId, anchor) &&
        !charRenderer->getInstanceModelMatrix(charInstanceId, anchor)) {
        return false;
    }
    // The size it is drawn at - the unit's own size times the effect's - is
    // held within what the effect allows, so a giant's aura does not fill the
    // zone and a critter's does not vanish.
    const float unitScale = glm::length(glm::vec3(anchor[0]));
    if (!(unitScale > 0.0f) || !std::isfinite(unitScale)) return false;
    const float drawn = std::clamp(unitScale * effect.scale, effect.minScale, effect.maxScale);
    out = anchor * glm::scale(glm::mat4(1.0f), glm::vec3(drawn / unitScale));
    return true;
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
            const StateKit::Effect& kitEffect = kitIt->second.effects[effect.effectIndex];
            glm::mat4 transform;
            if (!stateEffectTransform(state.instanceId, kitEffect, transform)) continue;
            if (effect.instanceId == 0) {
                if (spellVisualFailedModels_.count(effect.modelId)) continue;
                if (!ensureModel(kitEffect.path, effect.modelId)) continue;  // still loading
                // Created where it belongs and then given the whole matrix;
                // a child instance takes no part in placement dedup.
                effect.instanceId = m2Renderer_->createInstance(effect.modelId, glm::vec3(transform[3]),
                                                                glm::vec3(0.0f), 1.0f,
                                                                /*allowPositionDedup=*/false);
                if (effect.instanceId == 0) {
                    LOG_WARNING("SpellVisual: aura effect '", kitEffect.path, "' (visual ",
                                effect.visualId, ") could not be placed on its unit; not drawn");
                    spellVisualFailedModels_.insert(effect.modelId);
                    continue;
                }
                m2Renderer_->setInstanceTransform(effect.instanceId, transform);
                effect.lastTransform = transform;
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
