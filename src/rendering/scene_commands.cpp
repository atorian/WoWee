#include "rendering/scene_commands.hpp"

#include "core/logger.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/wmo_renderer.hpp"

namespace wowee {
namespace rendering {

size_t SceneCommands::apply() {
    // Instances asked for are made by the calls below; from here on their
    // renderers answer for them.
    const size_t made = calls_.apply(renderer_);
    characters_.queuedModel_.clear();
    models_.queuedModel_.clear();
    worldModels_.queued_.clear();
    return made;
}

void SceneCommands::discard() {
    const size_t dropped = calls_.discard();
    characters_.queuedModel_.clear();
    models_.queuedModel_.clear();
    worldModels_.queued_.clear();
    if (dropped > 0) {
        LOG_INFO("Scene: ", dropped, " changes asked for the world being left were dropped unmade");
    }
}

// --- Characters -----------------------------------------------------------

uint32_t SceneCommands::Characters::createInstance(uint32_t modelId, const glm::vec3& position,
                                                   const glm::vec3& rotation, float scale) {
    CharacterRenderer* cr = owner_.renderer_.getCharacterRenderer();
    if (!cr || !cr->hasModel(modelId)) {
        LOG_ERROR("Cannot create character instance: model ", modelId, " not loaded");
        return 0;
    }
    const uint32_t id = cr->reserveInstanceId();
    queuedModel_[id] = modelId;
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->createInstance(modelId, position, rotation, scale, id);
    });
    return id;
}

void SceneCommands::Characters::removeInstance(uint32_t instanceId) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->removeInstance(instanceId);
    });
}

void SceneCommands::Characters::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setInstancePosition(instanceId, position);
    });
}

void SceneCommands::Characters::setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setInstanceRotation(instanceId, rotation);
    });
}

void SceneCommands::Characters::setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setInstanceTorsoYaw(instanceId, deltaYawRad);
    });
}

void SceneCommands::Characters::setInstanceVisible(uint32_t instanceId, bool visible) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setInstanceVisible(instanceId, visible);
    });
}

void SceneCommands::Characters::setInstanceOpacity(uint32_t instanceId, float opacity) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setInstanceOpacity(instanceId, opacity);
    });
}

void SceneCommands::Characters::setInstanceStateLook(uint32_t instanceId, const glm::vec3& tint, float alpha) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setInstanceStateLook(instanceId, tint, alpha);
    });
}

void SceneCommands::Characters::startFadeIn(uint32_t instanceId, float durationSeconds) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->startFadeIn(instanceId, durationSeconds);
    });
}

void SceneCommands::Characters::playAnimation(uint32_t instanceId, uint32_t animationId, bool loop,
                                              uint32_t oneShotReturnAnim) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->playAnimation(instanceId, animationId, loop, oneShotReturnAnim);
    });
}

void SceneCommands::Characters::setInstanceAnimationSpeed(uint32_t instanceId, float speed) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setInstanceAnimationSpeed(instanceId, speed);
    });
}

void SceneCommands::Characters::setArmAnimations(uint32_t instanceId, uint32_t leftArmAnim,
                                                 uint32_t rightArmAnim) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setArmAnimations(instanceId, leftArmAnim, rightArmAnim);
    });
}

bool SceneCommands::Characters::attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                                             const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                                             const std::string& texturePath, const glm::mat4& localTransform) {
    CharacterRenderer* cr = owner_.renderer_.getCharacterRenderer();
    if (!cr) return false;
    const uint32_t charModel = modelOf(charInstanceId);
    if (charModel == 0) {
        LOG_WARNING("attachWeapon: character instance ", charInstanceId, " not found");
        return false;
    }
    if (!cr->hasAttachmentPoint(charModel, attachmentId)) {
        LOG_WARNING("attachWeapon: no bone found for attachment ", attachmentId);
        return false;
    }
    if (!cr->prepareWeaponModel(weaponModel, weaponModelId, texturePath)) return false;
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer())
            c->attachLoadedWeapon(charInstanceId, attachmentId, weaponModelId, localTransform);
    });
    return true;
}

void SceneCommands::Characters::detachWeapon(uint32_t charInstanceId, uint32_t attachmentId) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->detachWeapon(charInstanceId, attachmentId);
    });
}

bool SceneCommands::Characters::moveWeapon(uint32_t charInstanceId, uint32_t fromAttachmentId,
                                           uint32_t toAttachmentId, const glm::mat4& localTransform) {
    CharacterRenderer* cr = owner_.renderer_.getCharacterRenderer();
    const uint32_t charModel = modelOf(charInstanceId);
    if (!cr || charModel == 0) return false;
    if (!cr->hasAttachmentPoint(charModel, toAttachmentId)) {
        LOG_WARNING("moveWeapon: the model of instance ", charInstanceId, " has no attachment ",
                    toAttachmentId, "; the weapon stays at ", fromAttachmentId);
        return false;
    }
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer())
            c->moveWeapon(charInstanceId, fromAttachmentId, toAttachmentId, localTransform);
    });
    return true;
}

bool SceneCommands::Characters::attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId,
                                                   uint32_t visualSlot, const pipeline::M2Model& effectModel,
                                                   uint32_t effectModelId) {
    CharacterRenderer* cr = owner_.renderer_.getCharacterRenderer();
    if (!cr || modelOf(charInstanceId) == 0) return false;
    if (!cr->hasModel(effectModelId) && !cr->loadModel(effectModel, effectModelId)) {
        LOG_WARNING("attachWeaponEffect: failed to load effect model ", effectModelId);
        return false;
    }
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer())
            c->attachLoadedWeaponEffect(charInstanceId, attachmentId, visualSlot, effectModelId);
    });
    return true;
}

void SceneCommands::Characters::detachWeaponEffects(uint32_t charInstanceId, uint32_t attachmentId) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->detachWeaponEffects(charInstanceId, attachmentId);
    });
}

void SceneCommands::Characters::setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot,
                                                       TextureRef texture) {
    owner_.post([=, texture = std::move(texture)](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setTextureSlotOverride(instanceId, textureSlot, texture);
    });
}

void SceneCommands::Characters::clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->clearTextureSlotOverride(instanceId, textureSlot);
    });
}

void SceneCommands::Characters::setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup,
                                                        TextureRef texture) {
    owner_.post([=, texture = std::move(texture)](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setGroupTextureOverride(instanceId, geosetGroup, texture);
    });
}

void SceneCommands::Characters::setActiveGeosets(uint32_t instanceId,
                                                 const std::unordered_set<uint16_t>& geosets) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setActiveGeosets(instanceId, geosets);
    });
}

void SceneCommands::Characters::setDrawSkinExtra(uint32_t instanceId, bool enabled) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->setDrawSkinExtra(instanceId, enabled);
    });
}

void SceneCommands::Characters::addLookCompanion(uint32_t instanceId, uint32_t companionId) {
    owner_.post([=](Renderer& r) {
        if (auto* c = r.getCharacterRenderer()) c->addLookCompanion(instanceId, companionId);
    });
}

uint32_t SceneCommands::Characters::modelOf(uint32_t instanceId) const {
    if (const auto it = queuedModel_.find(instanceId); it != queuedModel_.end()) return it->second;
    const CharacterRenderer* cr = owner_.renderer_.getCharacterRenderer();
    return cr ? cr->getInstanceModelId(instanceId) : 0;
}

bool SceneCommands::Characters::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    const CharacterRenderer* cr = owner_.renderer_.getCharacterRenderer();
    const uint32_t model = modelOf(instanceId);
    return cr && model != 0 && cr->modelHasAnimation(model, animationId);
}

// --- Models (M2) ----------------------------------------------------------

uint32_t SceneCommands::Models::createInstance(uint32_t modelId, const glm::vec3& position,
                                               const glm::vec3& rotation, float scale) {
    M2Renderer* m2 = owner_.renderer_.getM2Renderer();
    if (!m2 || !m2->hasModel(modelId)) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    const uint32_t id = m2->reserveInstanceId();
    queuedModel_[id] = modelId;
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->createInstance(modelId, position, rotation, scale, false, id);
    });
    return id;
}

uint32_t SceneCommands::Models::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                                         const glm::vec3& position) {
    M2Renderer* m2 = owner_.renderer_.getM2Renderer();
    if (!m2 || !m2->hasModel(modelId)) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    const uint32_t id = m2->reserveInstanceId();
    queuedModel_[id] = modelId;
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->createInstanceWithMatrix(modelId, modelMatrix, position, id);
    });
    return id;
}

void SceneCommands::Models::removeInstance(uint32_t instanceId) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->removeInstance(instanceId);
    });
}

void SceneCommands::Models::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInstancePosition(instanceId, position);
    });
}

void SceneCommands::Models::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInstanceTransform(instanceId, transform);
    });
}

void SceneCommands::Models::setInstanceFade(uint32_t instanceId, float alpha) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInstanceFade(instanceId, alpha);
    });
}

void SceneCommands::Models::setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInstanceAnimation(instanceId, animationId, loop);
    });
}

void SceneCommands::Models::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInstanceAnimationFrozen(instanceId, frozen);
    });
}

void SceneCommands::Models::restartInstanceAnimation(uint32_t instanceId) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->restartInstanceAnimation(instanceId);
    });
}

bool SceneCommands::Models::playInstanceAnimation(uint32_t instanceId, const M2AnimationPlay& now,
                                                  const std::optional<M2AnimationPlay>& next) {
    if (!hasAnimation(instanceId, now.animationId)) return false;
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->playInstanceAnimation(instanceId, now, next);
    });
    return true;
}

void SceneCommands::Models::setInstanceHighlight(uint32_t instanceId, float amount) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInstanceHighlight(instanceId, amount);
    });
}

void SceneCommands::Models::clearInstanceHighlights() {
    owner_.post([](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->clearInstanceHighlights();
    });
}

void SceneCommands::Models::setSkipCollision(uint32_t instanceId, bool skip) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setSkipCollision(instanceId, skip);
    });
}

void SceneCommands::Models::setSkipWallCollision(uint32_t instanceId, bool skip) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setSkipWallCollision(instanceId, skip);
    });
}

void SceneCommands::Models::setInteriorLight(uint32_t instanceId, const std::optional<InteriorLight>& light) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInteriorLight(instanceId, light);
    });
}

void SceneCommands::Models::setInstanceIsGameObject(uint32_t instanceId, bool isGameObject) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->setInstanceIsGameObject(instanceId, isGameObject);
    });
}

uint32_t SceneCommands::Models::modelOf(uint32_t instanceId) const {
    if (const auto it = queuedModel_.find(instanceId); it != queuedModel_.end()) return it->second;
    const M2Renderer* m2 = owner_.renderer_.getM2Renderer();
    return m2 ? m2->getInstanceModelId(instanceId) : 0;
}

bool SceneCommands::Models::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    const M2Renderer* m2 = owner_.renderer_.getM2Renderer();
    const uint32_t model = modelOf(instanceId);
    return m2 && model != 0 && m2->modelHasAnimation(model, animationId);
}

float SceneCommands::Models::animationDuration(uint32_t instanceId, uint32_t animationId) const {
    const M2Renderer* m2 = owner_.renderer_.getM2Renderer();
    const uint32_t model = modelOf(instanceId);
    return m2 && model != 0 ? m2->modelAnimationDuration(model, animationId) : 0.0f;
}

void SceneCommands::Models::finishInstance(uint32_t instanceId) {
    owner_.post([=](Renderer& r) {
        if (auto* m = r.getM2Renderer()) m->finishInstance(instanceId);
    });
}

bool SceneCommands::Models::hasInstance(uint32_t instanceId) const {
    if (queuedModel_.count(instanceId)) return true;
    const M2Renderer* m2 = owner_.renderer_.getM2Renderer();
    return m2 && m2->hasInstance(instanceId);
}

// --- World models (WMO) ---------------------------------------------------

uint32_t SceneCommands::WorldModels::createInstance(uint32_t modelId, const glm::vec3& position,
                                                    const glm::vec3& rotation, float scale) {
    WMORenderer* wmo = owner_.renderer_.getWMORenderer();
    if (!wmo || !wmo->isModelLoaded(modelId)) {
        LOG_ERROR("Cannot create instance of unloaded WMO model ", modelId);
        return 0;
    }
    const uint32_t id = wmo->reserveInstanceId();
    queued_.insert(id);
    owner_.post([=](Renderer& r) {
        if (auto* w = r.getWMORenderer()) w->createInstance(modelId, position, rotation, scale, id);
    });
    return id;
}

bool SceneCommands::WorldModels::hasInstance(uint32_t instanceId) const {
    if (queued_.count(instanceId)) return true;
    const WMORenderer* wmo = owner_.renderer_.getWMORenderer();
    return wmo && wmo->hasInstance(instanceId);
}

void SceneCommands::WorldModels::removeInstance(uint32_t instanceId) {
    owner_.post([=](Renderer& r) {
        if (auto* w = r.getWMORenderer()) w->removeInstance(instanceId);
    });
}

void SceneCommands::WorldModels::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    owner_.post([=](Renderer& r) {
        if (auto* w = r.getWMORenderer()) w->setInstanceTransform(instanceId, transform);
    });
}

void SceneCommands::WorldModels::addDoodadToInstance(uint32_t instanceId, uint32_t m2InstanceId,
                                                     const glm::mat4& localTransform) {
    owner_.post([=](Renderer& r) {
        if (auto* w = r.getWMORenderer()) w->addDoodadToInstance(instanceId, m2InstanceId, localTransform);
    });
}

void SceneCommands::WorldModels::setInstanceIsTransport(uint32_t instanceId, bool isTransport) {
    owner_.post([=](Renderer& r) {
        if (auto* w = r.getWMORenderer()) w->setInstanceIsTransport(instanceId, isTransport);
    });
}

void SceneCommands::WorldModels::setInstanceHidden(uint32_t instanceId, bool hidden) {
    owner_.post([=](Renderer& r) {
        if (auto* w = r.getWMORenderer()) w->setInstanceHidden(instanceId, hidden);
    });
}

size_t SceneCommands::WorldModels::setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId,
                                                              bool loop) {
    owner_.post([=](Renderer& r) {
        if (auto* w = r.getWMORenderer()) w->setInstanceDoodadAnimation(instanceId, animationId, loop);
    });
    const WMORenderer* wmo = owner_.renderer_.getWMORenderer();
    return wmo ? wmo->instanceDoodadCount(instanceId) : 0;
}

} // namespace rendering
} // namespace wowee
