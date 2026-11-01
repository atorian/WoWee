#include "core/entity_spawn_callback_handler.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/game_events.hpp"

namespace wowee { namespace core {

EntitySpawnCallbackHandler::EntitySpawnCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer* renderer,
    EventBus& events,
    std::function<bool(uint64_t)> isLocalPlayerGuid)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , events_(events)
    , isLocalPlayerGuid_(std::move(isLocalPlayerGuid))
{
}

void EntitySpawnCallbackHandler::setupCallbacks() {
    // Creature spawn callback (online mode) - spawn creature models
    subscriptions_.push_back(events_.subscribe<game::events::CreatureSpawned>(
        [this](const game::events::CreatureSpawned& e) {
            // Queue spawns to avoid hanging when many creatures appear at once.
            // A VALUES update can change a creature's display in place (Westfall
            // lumberjacks alternate between the normal and log-carrying models).
            // Replace the old render instance instead of retaining stale geometry.
            // One held across a reconnect is the same creature again.
            entitySpawner_.shown(e.guid);
            if (entitySpawner_.isCreatureSpawned(e.guid)) {
                const auto& displayIds = entitySpawner_.getCreatureDisplayIds();
                auto it = displayIds.find(e.guid);
                if (it != displayIds.end() && it->second == e.displayId) return;
                uint32_t retainedEmote = 0;
                auto& activeEmotes = entitySpawner_.getCreatureActiveEmotes();
                if (auto emoteIt = activeEmotes.find(e.guid); emoteIt != activeEmotes.end()) {
                    retainedEmote = emoteIt->second;
                }
                entitySpawner_.despawnCreature(e.guid);
                if (retainedEmote != 0) activeEmotes[e.guid] = retainedEmote;
            }
            entitySpawner_.queueCreatureSpawn(e.guid, e.displayId, e.x, e.y, e.z, e.orientation, e.scale);
        }));

    // Player spawn callback (online mode) - spawn player models with correct textures
    subscriptions_.push_back(events_.subscribe<game::events::PlayerSpawned>(
        [this](const game::events::PlayerSpawned& e) {
            LOG_DEBUG("PlayerSpawned: guid=0x", std::hex, e.guid, std::dec,
                      " race=", static_cast<int>(e.raceId), " gender=", static_cast<int>(e.genderId),
                      " pos=(", e.x, ",", e.y, ",", e.z, ")");
            // Skip local player - already spawned as the main character
            if (isLocalPlayerGuid_(e.guid)) return;
            entitySpawner_.shown(e.guid);
            if (entitySpawner_.isPlayerSpawned(e.guid)) return;
            if (entitySpawner_.isPlayerPending(e.guid)) return;
            entitySpawner_.queuePlayerSpawn(e.guid, e.raceId, e.genderId, e.appearanceBytes, e.facialFeatures, e.x, e.y, e.z, e.orientation);
        }));

    // Online player equipment callback - apply armor geosets/skin overlays per player instance.
    subscriptions_.push_back(events_.subscribe<game::events::PlayerEquipmentResolved>(
        [this](const game::events::PlayerEquipmentResolved& e) {
            // Queue equipment compositing instead of doing it immediately -
            // compositeWithRegions is expensive (file I/O + CPU blit + GPU upload)
            // and causes frame stutters if multiple players update at once.
            entitySpawner_.queuePlayerEquipment(
                e.guid, {.displayInfoIds = e.displayIds, .inventoryTypes = e.inventoryTypes,
                         .enchantIds = e.enchantIds});
        }));

    subscriptions_.push_back(events_.subscribe<game::events::OtherPlayerMounted>(
        [this](const game::events::OtherPlayerMounted& e) {
            if (isLocalPlayerGuid_(e.guid)) return;
            entitySpawner_.setRemotePlayerMountDisplayId(e.guid, e.mountDisplayId);
        }));

    // Creature despawn callback (online mode) - remove creature models
    subscriptions_.push_back(events_.subscribe<game::events::CreatureDespawned>(
        [this](const game::events::CreatureDespawned& e) {
            entitySpawner_.despawnCreature(e.guid);
        }));

    subscriptions_.push_back(events_.subscribe<game::events::PlayerDespawned>(
        [this](const game::events::PlayerDespawned& e) {
            entitySpawner_.despawnPlayer(e.guid);
        }));

    // GameObject spawn callback (online mode) - spawn static models (mailboxes, etc.)
    subscriptions_.push_back(events_.subscribe<game::events::GameObjectSpawned>(
        [this](const game::events::GameObjectSpawned& e) {
            entitySpawner_.shown(e.guid);
            entitySpawner_.queueGameObjectSpawn(e.guid, e.entry, e.displayId, e.x, e.y, e.z, e.orientation, e.scale);
        }));

    // GameObject despawn callback (online mode) - remove static models
    subscriptions_.push_back(events_.subscribe<game::events::GameObjectDespawned>(
        [this](const game::events::GameObjectDespawned& e) {
            entitySpawner_.despawnGameObject(e.guid);
        }));

    // GameObject custom animation callback (e.g. chest opening)
    // Without a map scope there is no scene to animate; the scope presents
    // each object as the entities have it when it is built.
    subscriptions_.push_back(events_.subscribe<game::events::GameObjectCustomAnimPlayed>(
        [this](const game::events::GameObjectCustomAnimPlayed& e) {
            if (!entitySpawner_.hasMap()) return;
            auto& goInstances = entitySpawner_.getGameObjectInstances();
            auto it = goInstances.find(e.guid);
            if (it == goInstances.end()) return;
            auto& info = it->second;
            if (!info.isWmo && renderer_) {
                auto& models = renderer_->scene().models();
                // Play the custom animation as a one-shot if model supports it
                if (models.hasAnimation(info.instanceId, e.animId))
                    models.setInstanceAnimation(info.instanceId, e.animId, false);
                else
                    models.setInstanceAnimationFrozen(info.instanceId, false);
            }
        }));

    // GameObject state change callback: a door, a chest, a destructible is a
    // pose the server describes, and the spawner holds it - including the one
    // that arrives before the model does.
    subscriptions_.push_back(events_.subscribe<game::events::GameObjectStateChanged>(
        [this](const game::events::GameObjectStateChanged& e) {
            entitySpawner_.applyGameObjectState(e.guid, e.goState);
        }));

    // Creature move callback (online mode). Position is evaluated from the
    // Entity - its server spline, or a player's reported movement - in
    // Application::update; starting a second renderer interpolation here made
    // the two owners chase each other. What is left is a creature's
    // locomotion animation for the spline it was just given. Another player's
    // animation follows its own reported movement in the frame sync.
    subscriptions_.push_back(events_.subscribe<game::events::CreatureMoved>(
        [this](const game::events::CreatureMoved& e) {
            auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
            if (!cr || e.durationMs == 0) return;
            const uint32_t instanceId = entitySpawner_.getCreatureInstanceId(e.guid);
            if (instanceId == 0) return;
            // Monster patrols report Walk through SMSG_MONSTER_MOVE's spline flags
            // before this callback runs. Don't override Death; the per-frame sync
            // returns to Stand when movement stops.
            uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
            const bool gotState = cr->getAnimationState(instanceId, curAnimId, curT, curDur);
            const bool walking = entitySpawner_.getCreatureWalkingState().count(e.guid) > 0;
            const uint32_t targetAnim = walking ? rendering::anim::WALK : rendering::anim::RUN;
            // Only restart when the selected locomotion animation changed.
            if (!gotState || (curAnimId != rendering::anim::DEATH && curAnimId != targetAnim)) {
                renderer_->scene().characters().playAnimation(instanceId, targetAnim, /*loop=*/true);
            }
            entitySpawner_.getCreatureWasMoving()[e.guid] = true;
        }));

    subscriptions_.push_back(events_.subscribe<game::events::GameObjectMoved>(
        [this](const game::events::GameObjectMoved& e) {
            if (!entitySpawner_.hasMap()) return;
            auto& goInstMap = entitySpawner_.getGameObjectInstances();
            auto it = goInstMap.find(e.guid);
            if (it == goInstMap.end()) {
                return;
            }
            glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(e.x, e.y, e.z));
            auto& info = it->second;
            if (!renderer_) return;
            if (info.isWmo) {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                transform = glm::rotate(transform, e.orientation, glm::vec3(0, 0, 1));
                renderer_->scene().worldModels().setInstanceTransform(info.instanceId, transform);
            } else {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                renderer_->scene().models().setInstanceTransform(info.instanceId, transform);
            }
        }));
}

}} // namespace wowee::core
