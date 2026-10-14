#include "core/spell_presentation.hpp"

#include "core/coordinates.hpp"
#include "core/entity_spawner.hpp"
#include "core/env_flag.hpp"
#include "core/logger.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "game/entity.hpp"
#include "game/game_events.hpp"
#include "game/game_handler.hpp"
#include "game/spell_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/spell_visual_system.hpp"

#include <algorithm>

namespace wowee::core {

namespace {

audio::SpellSoundManager::MagicSchool magicSchool(uint32_t schoolMask) {
    using School = audio::SpellSoundManager::MagicSchool;
    if (schoolMask & 0x04) return School::FIRE;
    if (schoolMask & 0x10) return School::FROST;
    if (schoolMask & 0x02) return School::HOLY;
    if (schoolMask & 0x08) return School::NATURE;
    if (schoolMask & 0x20) return School::SHADOW;
    return School::ARCANE;
}

} // namespace

SpellPresentation::SpellPresentation(EventBus& events, game::GameHandler& game,
                                     rendering::Renderer& renderer, EntitySpawner& spawner,
                                     rendering::SpellVisualSystem* visuals,
                                     audio::AudioCoordinator* audio,
                                     pipeline::AssetManager& assets)
    : game_(game), renderer_(renderer), spawner_(spawner), visuals_(visuals),
      audio_(audio), assets_(assets) {
    namespace ev = game::events;
    subscriptions_.push_back(events.subscribe<ev::SpellCastStarted>([this](const ev::SpellCastStarted& e) {
        onCastStarted(e.casterGuid, e.spellId, e.castTimeMs, e.byPlayer, e.isProfession,
                      e.isRangedAutoAttack);
    }));
    subscriptions_.push_back(events.subscribe<ev::SpellCastSucceeded>([this](const ev::SpellCastSucceeded& e) {
        onCastSucceeded(e.casterGuid, e.spellId, e.byPlayer, e.isProfession, e.isRangedAutoAttack,
                        e.isMeleeAbility, e.hitTargets, e.projectileTarget);
    }));
    subscriptions_.push_back(events.subscribe<ev::PlayerCastStopped>([this](const ev::PlayerCastStopped& e) {
        if (visuals_) visuals_->cancelAllPrecastVisuals();
        if (!e.failed || !audio_) return;
        if (auto* ssm = audio_->getSpellSoundManager()) ssm->stopPrecast();
    }));
    subscriptions_.push_back(events.subscribe<ev::PlayerCastFailed>([this](const ev::PlayerCastFailed& e) {
        if (e.routine || !audio_) return;
        if (auto* sfx = audio_->getUiSoundManager()) sfx->playError();
    }));
    subscriptions_.push_back(events.subscribe<ev::SpellVisualRequested>([this](const ev::SpellVisualRequested& e) {
        if (!visuals_ || e.visualId == 0) return;
        glm::vec3 pos;
        if (!unitPosition(e.unitGuid, pos)) return;
        // On the unit, so that it rides the unit's attachments: an impact
        // landing on a running player runs with them.
        visuals_->playSpellVisual(e.visualId, pos, e.impactKit, unitInstance(e.unitGuid));
    }));
    // The character's own effects, loaded in the background rather than in the
    // frame each spell is first cast.
    subscriptions_.push_back(events.subscribe<ev::SpellsLearned>([this](const ev::SpellsLearned& e) {
        if (!visuals_) return;
        std::vector<uint32_t> visualIds;
        visualIds.reserve(e.spellIds.size());
        for (const uint32_t spellId : e.spellIds) {
            if (const uint32_t visualId = visualIdFor(spellId)) visualIds.push_back(visualId);
        }
        std::sort(visualIds.begin(), visualIds.end());
        visualIds.erase(std::unique(visualIds.begin(), visualIds.end()), visualIds.end());
        visuals_->preloadVisuals(visualIds);
    }));
}

void SpellPresentation::update(float deltaTime) {
    syncAuraLooks(deltaTime);
    tickRestorationSound(deltaTime);
}

void SpellPresentation::onCastStarted(uint64_t caster, uint32_t spellId, uint32_t castTimeMs,
                                      bool byPlayer, bool isProfession, bool isRangedAutoAttack) {
    if (isProfession || isRangedAutoAttack) return;

    if (byPlayer && castTimeMs > 0 && audio_) {
        if (auto* ssm = audio_->getSpellSoundManager()) {
            ssm->playPrecast(magicSchool(game_.getSpellSchoolMask(spellId)),
                             audio::SpellSoundManager::SpellPower::MEDIUM);
        }
    }

    // The precast/cast kit at the caster.
    if (!visuals_) return;
    const uint32_t visualId = visualIdFor(spellId);
    if (visualId == 0) {
        LOG_WARNING("SpellVisual: no visual for spell ", spellId, " - its cast is not drawn");
        return;
    }
    glm::vec3 casterPos;
    if (!unitPosition(caster, casterPos)) {
        LOG_DEBUG("SpellVisual: caster 0x", std::hex, caster, std::dec,
                  " is not drawn - no cast effect for spell ", spellId);
        return;
    }
    visuals_->playSpellVisualPrecast(visualId, casterPos, castTimeMs, unitInstance(caster));
}

void SpellPresentation::onCastSucceeded(uint64_t caster, uint32_t spellId, bool byPlayer,
                                        bool isProfession, bool isRangedAutoAttack,
                                        bool isMeleeAbility,
                                        const std::vector<uint64_t>& hitTargets,
                                        uint64_t projectileTarget) {
    const uint64_t player = game_.getPlayerGuid();
    if (byPlayer) {
        if (!isProfession && !isRangedAutoAttack) playCastSound(spellId);
        if (isRangedAutoAttack) launchRangedProjectile(spellId, projectileTarget);
        if (isMeleeAbility && audio_) {
            if (auto* csm = audio_->getCombatSoundManager()) {
                csm->playWeaponSwing(audio::CombatSoundManager::WeaponSize::MEDIUM, false);
                csm->playImpact(audio::CombatSoundManager::WeaponSize::MEDIUM,
                                audio::CombatSoundManager::ImpactType::FLESH, false);
            }
        }
    } else if (!isRangedAutoAttack &&
               std::find(hitTargets.begin(), hitTargets.end(), player) != hitTargets.end()) {
        playCastSound(spellId);
    }

    // The impact is heard when it lands on the player, or the player's lands.
    bool playerIsHit = false;
    bool playerHitEnemy = false;
    for (const uint64_t target : hitTargets) {
        if (target == player) playerIsHit = true;
        if (caster == player && target != player && target != 0) playerHitEnemy = true;
    }
    if ((playerIsHit || playerHitEnemy) && !isRangedAutoAttack) playImpactSound(spellId);

    // The cast kit at the caster - instants have no SPELL_START to show it - and
    // the impact kit at each target.
    if (isProfession || isRangedAutoAttack || !visuals_) return;
    const uint32_t visualId = visualIdFor(spellId);
    if (visualId == 0) return;
    glm::vec3 casterPos;
    if (unitPosition(caster, casterPos)) {
        visuals_->playSpellVisual(visualId, casterPos, /*useImpactKit=*/false, unitInstance(caster));
    }
    for (const uint64_t target : hitTargets) {
        if (target != 0) playImpactVisual(spellId, target);
    }
}

void SpellPresentation::syncAuraLooks(float deltaTime) {
    // A few times a second is soon enough for an aura to show, and the units
    // and their auras are walked only then.
    // WOWEE_AURA_LOOKS=0 turns them off, to tell an aura's look apart from
    // anything else wrong with how a unit is drawn.
    static const bool enabled = envFlagEnabled("WOWEE_AURA_LOOKS", true);
    if (!enabled || !visuals_) return;
    auraLookTimer_ += deltaTime;
    if (auraLookTimer_ < 0.2f) return;
    auraLookTimer_ = 0.0f;

    const auto& spells = *game_.getSpellHandler();
    std::vector<rendering::SpellVisualSystem::UnitStateVisuals> units;
    units.reserve(spells.getUnitAurasByGuid().size() + 1);
    auto addUnit = [&](uint64_t guid, const std::vector<game::AuraSlot>& auras) {
        const uint32_t instanceId = unitInstance(guid);
        if (instanceId == 0) return;
        rendering::SpellVisualSystem::UnitStateVisuals unit{.guid = guid, .instanceId = instanceId};
        for (const auto& aura : auras) {
            if (aura.isEmpty()) continue;
            if (const uint32_t visualId = visualIdFor(aura.spellId)) unit.visualIds.push_back(visualId);
        }
        units.push_back(std::move(unit));
    };
    if (const uint64_t player = game_.getPlayerGuid()) addUnit(player, spells.getPlayerAuras());
    // Every other unit's list is kept by guid, the target's included.
    for (const auto& [guid, auras] : spells.getUnitAurasByGuid()) addUnit(guid, auras);
    visuals_->syncStateVisuals(units);
}

void SpellPresentation::tickRestorationSound(float deltaTime) {
    // Eating and drinking munch and gulp for the whole restoration aura,
    // matching the real client's looping consume sound - the first at once.
    const auto& spells = *game_.getSpellHandler();
    const bool restoring = spells.isRestoring();
    if (restoring && !wasRestoring_) restorationSoundTimer_ = 0.0f;
    wasRestoring_ = restoring;
    if (!restoring) return;
    restorationSoundTimer_ -= deltaTime;
    if (restorationSoundTimer_ > 0.0f) return;
    restorationSoundTimer_ = 1.0f;
    if (!audio_) return;
    if (auto* sfx = audio_->getUiSoundManager()) {
        if (spells.isRestorationFood()) sfx->playEating();
        else                            sfx->playDrinking();
    }
}

void SpellPresentation::playCastSound(uint32_t spellId) {
    if (!audio_) return;
    if (auto* ssm = audio_->getSpellSoundManager())
        ssm->playCast(magicSchool(game_.getSpellSchoolMask(spellId)));
}

void SpellPresentation::playImpactSound(uint32_t spellId) {
    if (!audio_) return;
    if (auto* ssm = audio_->getSpellSoundManager())
        ssm->playImpact(magicSchool(game_.getSpellSchoolMask(spellId)),
                        audio::SpellSoundManager::SpellPower::MEDIUM);
}

void SpellPresentation::playImpactVisual(uint32_t spellId, uint64_t target) {
    const uint32_t visualId = visualIdFor(spellId);
    if (visualId == 0) {
        LOG_WARNING("SpellVisual: no visual for spell ", spellId, " - its impact is not drawn");
        return;
    }
    glm::vec3 targetPos;
    if (!unitPosition(target, targetPos)) return;
    // On the target, so that it rides the target's attachments: a buff
    // landing on a running player runs with them.
    visuals_->playSpellVisual(visualId, targetPos, /*useImpactKit=*/true, unitInstance(target));
}

void SpellPresentation::launchRangedProjectile(uint32_t spellId, uint64_t target) {
    if (target == 0) target = game_.getTargetGuid();
    if (target == 0 || !visuals_) return;
    auto* characters = renderer_.getCharacterRenderer();
    if (!characters) return;

    glm::vec3 start = renderer_.getCharacterPosition() + glm::vec3(0.0f, 0.0f, 1.0f);
    glm::mat4 handTransform(1.0f);
    if (characters->getAttachmentTransform(renderer_.getCharacterInstanceId(), 1, handTransform))
        start = glm::vec3(handTransform[3]);

    glm::vec3 end;
    if (!unitPosition(target, end)) return;
    end.z += 1.0f;

    const auto& ranged = game_.getInventory().getEquipSlot(game::EquipSlot::RANGED);
    std::string modelPath;
    std::string texturePath;
    bool spin = false;

    if (spellId == 2764 || (!ranged.empty() && ranged.item.inventoryType == game::InvType::THROWN)) {
        spin = true;
        if (!ranged.empty() && ranged.item.displayInfoId != 0) {
            auto displayDbc = assets_.loadDBC("ItemDisplayInfo.dbc");
            if (displayDbc && displayDbc->isLoaded()) {
                const int32_t row = displayDbc->findRecordById(ranged.item.displayInfoId);
                if (row >= 0) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                    std::string model = displayDbc->getString(
                        static_cast<uint32_t>(row), layout ? (*layout)["LeftModel"] : 1);
                    std::string texture = displayDbc->getString(
                        static_cast<uint32_t>(row), layout ? (*layout)["LeftModelTexture"] : 3);
                    if (!model.empty()) {
                        const size_t dot = model.rfind('.');
                        if (dot != std::string::npos) model = model.substr(0, dot);
                        modelPath = "Item\\ObjectComponents\\Weapon\\" + model + ".m2";
                    }
                    if (!texture.empty())
                        texturePath = "Item\\ObjectComponents\\Weapon\\" + texture + ".blp";
                }
            }
        }
        if (modelPath.empty())
            modelPath = "Item\\ObjectComponents\\Weapon\\Thrown_1H_Dagger_A_01.m2";
    } else if (spellId == 75 || (!ranged.empty() &&
               (ranged.item.inventoryType == game::InvType::RANGED_BOW ||
                ranged.item.subclassName == "Bow" || ranged.item.subclassName == "Crossbow"))) {
        modelPath = "Item\\ObjectComponents\\Ammo\\ArrowFlight_01.m2";
    } else {
        modelPath = "Item\\ObjectComponents\\Ammo\\BulletFlight_01.m2";
    }

    const float distance = glm::length(end - start);
    const float duration = std::clamp(distance / 35.0f, 0.12f, 0.8f);
    visuals_->playPhysicalProjectile(modelPath, texturePath, start, end, duration, spin);
}

uint32_t SpellPresentation::visualIdFor(uint32_t spellId) const {
    return game_.getSpellVisualId(spellId);
}

bool SpellPresentation::unitPosition(uint64_t guid, glm::vec3& out) const {
    if (guid == game_.getPlayerGuid()) {
        out = renderer_.getCharacterPosition();
        return true;
    }
    auto entity = game_.getEntityManager().getEntity(guid);
    if (!entity) return false;
    const glm::vec3 canonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
    out = coords::canonicalToRender(canonical);
    return true;
}

uint32_t SpellPresentation::unitInstance(uint64_t guid) const {
    if (guid == game_.getPlayerGuid()) return renderer_.getCharacterInstanceId();
    uint32_t instanceId = spawner_.getCreatureInstanceId(guid);
    if (instanceId == 0) instanceId = spawner_.getPlayerInstanceId(guid);
    return instanceId;
}

} // namespace wowee::core
