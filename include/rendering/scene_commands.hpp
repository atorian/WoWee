#pragma once

#include "rendering/deferred_calls.hpp"
#include "rendering/game_object_animation.hpp"
#include "rendering/interior_light.hpp"
#include "rendering/texture_cache.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace pipeline { struct M2Model; }
namespace rendering {

class Renderer;

/// The scene's one way in from outside the renderer: every change to what is
/// drawn - an instance made, moved, shown, hidden, animated, dressed, taken
/// away - is asked for here and made only inside the frame's Present phase
/// (apply): at its start, before the player's controls stand on this frame's
/// transports, and again at the start of the renderer's update, for what
/// Present itself asked - the player's own model following the camera. From
/// there to the end of the frame the renderers' instances are only read.
///
/// The client asked the renderers directly from wherever it was: packet
/// handlers, the spawner, the interface, the camera - some 200 calls across
/// six phases of the frame. A renderer working from a snapshot on its own
/// thread needs the scene to change in one place; this is that place, a step
/// before the snapshot exists.
///
/// An instance asked for has its id at once, reserved from its renderer, and
/// exists from the apply point. What a caller must know straight away - does
/// the model have this animation, this attachment point, does the weapon's
/// model load - is answered now, from the models, also for an instance whose
/// creation is still queued. Loading models and textures is not a change to
/// the scene and stays where it is, as do the queries of where things are.
///
/// Main thread only.
class SceneCommands {
public:
    explicit SceneCommands(Renderer& renderer) : renderer_(renderer) {}

    /// Makes every change asked for, in the order asked. Returns how many.
    size_t apply();
    /// Drops every change asked for and not yet made: the world they were
    /// for is going (a map change, leaving the world). Ids handed out for
    /// instances still to be made name nothing.
    void discard();
    [[nodiscard]] size_t pending() const { return calls_.size(); }

    /// Characters, creatures and what they hold (CharacterRenderer).
    class Characters {
    public:
        /// 0, and logged, when the model is not loaded.
        uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                                const glm::vec3& rotation = glm::vec3(0.0f), float scale = 1.0f);
        void removeInstance(uint32_t instanceId);
        /// Lets the model go once nothing shows it - after the removals
        /// asked for before this one.
        void unloadModelIfUnused(uint32_t modelId);

        void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
        void setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation);
        void setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad);
        void setInstanceVisible(uint32_t instanceId, bool visible);
        void setInstanceOpacity(uint32_t instanceId, float opacity);
        void startFadeIn(uint32_t instanceId, float durationSeconds);
        void playAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true,
                           uint32_t oneShotReturnAnim = 0);
        void setInstanceAnimationSpeed(uint32_t instanceId, float speed);

        /// False, and logged, when the character's model has no such point
        /// or the weapon's model does not load; the weapon is loaded now and
        /// hung on the instance at the apply point.
        bool attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                          const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                          const std::string& texturePath,
                          const glm::mat4& localTransform = glm::mat4(1.0f));
        void detachWeapon(uint32_t charInstanceId, uint32_t attachmentId);
        /// False, and logged, when the character's model has no `to` point.
        bool moveWeapon(uint32_t charInstanceId, uint32_t fromAttachmentId, uint32_t toAttachmentId,
                        const glm::mat4& localTransform);
        /// False when the effect's model does not load. Hung at the apply
        /// point on the weapon there is then at the attachment.
        bool attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId, uint32_t visualSlot,
                                const pipeline::M2Model& effectModel, uint32_t effectModelId);
        void detachWeaponEffects(uint32_t charInstanceId, uint32_t attachmentId);

        void setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, TextureRef texture);
        void clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot);
        void setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, TextureRef texture);
        void setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets);
        void setDrawSkinExtra(uint32_t instanceId, bool enabled);
        void addLookCompanion(uint32_t instanceId, uint32_t companionId);

        /// The model an instance shows, a queued one's included; 0 for none.
        [[nodiscard]] uint32_t modelOf(uint32_t instanceId) const;
        /// Whether the instance's model has the animation, a queued one's included.
        [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;

    private:
        friend class SceneCommands;
        explicit Characters(SceneCommands& owner) : owner_(owner) {}
        SceneCommands& owner_;
        std::unordered_map<uint32_t, uint32_t> queuedModel_;
    };

    /// Doodads, game objects and effects (M2Renderer).
    class Models {
    public:
        /// 0, and logged, when the model is not loaded. Never folded into
        /// another instance at the same spot.
        uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                                const glm::vec3& rotation = glm::vec3(0.0f), float scale = 1.0f);
        uint32_t createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                          const glm::vec3& position);
        void removeInstance(uint32_t instanceId);

        void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
        void setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true);
        void setInstanceAnimationFrozen(uint32_t instanceId, bool frozen);
        void restartInstanceAnimation(uint32_t instanceId);
        /// False when the instance's model has no such sequence.
        bool playInstanceAnimation(uint32_t instanceId, const M2AnimationPlay& now,
                                   const std::optional<M2AnimationPlay>& next = std::nullopt);
        void setInstanceHighlight(uint32_t instanceId, float amount);
        void clearInstanceHighlights();
        void setSkipCollision(uint32_t instanceId, bool skip);
        void setSkipWallCollision(uint32_t instanceId, bool skip);
        void setInteriorLight(uint32_t instanceId, const std::optional<InteriorLight>& light);
        void setInstanceIsGameObject(uint32_t instanceId, bool isGameObject);

        [[nodiscard]] uint32_t modelOf(uint32_t instanceId) const;
        [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
        /// Whether the instance exists, or is queued to.
        [[nodiscard]] bool hasInstance(uint32_t instanceId) const;

    private:
        friend class SceneCommands;
        explicit Models(SceneCommands& owner) : owner_(owner) {}
        SceneCommands& owner_;
        std::unordered_map<uint32_t, uint32_t> queuedModel_;
    };

    /// Buildings and transports (WMORenderer).
    class WorldModels {
    public:
        /// 0, and logged, when the model is not loaded.
        uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                                const glm::vec3& rotation = glm::vec3(0.0f), float scale = 1.0f);
        void removeInstance(uint32_t instanceId);
        void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
        void addDoodadToInstance(uint32_t instanceId, uint32_t m2InstanceId, const glm::mat4& localTransform);
        void setInstanceIsTransport(uint32_t instanceId, bool isTransport);
        void setInstanceHidden(uint32_t instanceId, bool hidden);
        /// Returns how many doodads the instance has now; the animation is
        /// set on them at the apply point.
        size_t setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId, bool loop);
        /// Whether the instance exists, or is queued to.
        [[nodiscard]] bool hasInstance(uint32_t instanceId) const;

    private:
        friend class SceneCommands;
        explicit WorldModels(SceneCommands& owner) : owner_(owner) {}
        SceneCommands& owner_;
        std::unordered_set<uint32_t> queued_;
    };

    Characters& characters() { return characters_; }
    Models& models() { return models_; }
    WorldModels& worldModels() { return worldModels_; }

private:
    void post(DeferredCalls<Renderer>::Call call) { calls_.post(std::move(call)); }

    Renderer& renderer_;
    DeferredCalls<Renderer> calls_;
    Characters characters_{*this};
    Models models_{*this};
    WorldModels worldModels_{*this};
};

} // namespace rendering
} // namespace wowee
