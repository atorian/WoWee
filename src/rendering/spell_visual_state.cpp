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
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <span>
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
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
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
        LOG_INFO("SpellVisual: no StateKit column in this expansion's layout; aura looks are off");
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

    // SpellVisualEffectName: id -> model and its scale.
    struct EffectName { std::string path; float scale = 1.0f; };
    std::unordered_map<uint32_t, EffectName> effectNames;
    {
        const uint32_t pathField  = fxLayout->tryField("FilePath");
        const uint32_t scaleField = fxLayout->tryField("Scale");
        const uint32_t fc = fxDbc->getFieldCount();
        if (pathField >= fc) return;
        for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
            const uint32_t id = fxDbc->getUInt32(i, 0);
            std::string path = fxDbc->getString(i, pathField);
            if (!id || path.empty()) continue;
            float scale = scaleField < fc ? fxDbc->getFloat(i, scaleField) : 1.0f;
            if (!std::isfinite(scale) || scale <= 0.0f) scale = 1.0f;
            effectNames[id] = {.path = pipeline::modelPathToM2(path),
                               .scale = std::clamp(scale, 0.05f, 20.0f)};
        }
    }

    // SpellVisualKit: id -> what the kit hangs on the unit and does to it.
    struct SlotField { const char* name; KitSlot slot; };
    static constexpr SlotField kSlotFields[] = {
        {.name = "HeadEffect",      .slot = KitSlot::Head},
        {.name = "ChestEffect",     .slot = KitSlot::Chest},
        {.name = "BaseEffect",      .slot = KitSlot::Base},
        {.name = "LeftHandEffect",  .slot = KitSlot::LeftHand},
        {.name = "RightHandEffect", .slot = KitSlot::RightHand},
        {.name = "BreathEffect",    .slot = KitSlot::Breath},
        {.name = "SpecialEffect0",  .slot = KitSlot::Special},
        {.name = "SpecialEffect1",  .slot = KitSlot::Special},
        {.name = "SpecialEffect2",  .slot = KitSlot::Special},
    };
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
        for (const auto& sf : kSlotFields) {
            const uint32_t field = kitLayout->tryField(sf.name);
            if (field >= kitFc) continue;
            const uint32_t effectId = kitDbc->getUInt32(i, field);
            const auto it = effectNames.find(effectId);
            if (it == effectNames.end()) continue;
            kit.effects.push_back({.path = it->second.path, .slot = sf.slot, .scale = it->second.scale});
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

bool SpellVisualSystem::stateEffectPosition(uint32_t charInstanceId, KitSlot slot, glm::vec3& out) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer) return false;
    glm::vec3 feet;
    if (!charRenderer->getInstancePosition(charInstanceId, feet)) return false;

    // M2 attachment ids, in the order the client's own would be tried.
    static constexpr uint32_t kHead[]  = {11};      // Helm
    static constexpr uint32_t kChest[] = {15};      // ChestBloodFront
    static constexpr uint32_t kLeft[]  = {21, 2};   // SpellLeftHand, HandLeft
    static constexpr uint32_t kRight[] = {22, 1};   // SpellRightHand, HandRight
    static constexpr uint32_t kBreath[] = {17};     // Breath
    std::span<const uint32_t> attachments;
    float heightFraction = 0.0f;  // where on the body, when the model has no attachment
    switch (slot) {
        case KitSlot::Base:
        case KitSlot::Special:   out = feet; return true;
        case KitSlot::Head:      attachments = kHead;   heightFraction = 0.92f; break;
        case KitSlot::Chest:     attachments = kChest;  heightFraction = 0.62f; break;
        case KitSlot::LeftHand:  attachments = kLeft;   heightFraction = 0.45f; break;
        case KitSlot::RightHand: attachments = kRight;  heightFraction = 0.45f; break;
        case KitSlot::Breath:    attachments = kBreath; heightFraction = 0.85f; break;
    }
    for (const uint32_t attachmentId : attachments) {
        glm::mat4 transform;
        if (charRenderer->getAttachmentTransform(charInstanceId, attachmentId, transform)) {
            out = glm::vec3(transform[3]);
            return true;
        }
    }
    float height = 2.0f;
    charRenderer->getInstanceHeight(charInstanceId, height);
    out = feet + glm::vec3(0.0f, 0.0f, height * heightFraction);
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
            glm::vec3 position;
            if (!stateEffectPosition(state.instanceId, kitEffect.slot, position)) continue;
            if (effect.instanceId == 0) {
                if (spellVisualFailedModels_.count(effect.modelId)) continue;
                if (!ensureModel(kitEffect.path, effect.modelId)) continue;  // still loading
                effect.instanceId = m2Renderer_->createInstance(effect.modelId, position,
                                                                glm::vec3(0.0f), kitEffect.scale);
                effect.lastPosition = position;
            } else if (position != effect.lastPosition) {
                // Only when the unit moved: a move refiles the instance in
                // the culling grids, and most units stand still.
                m2Renderer_->setInstancePosition(effect.instanceId, position);
                effect.lastPosition = position;
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
