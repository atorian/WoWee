#include "game/world_handler.hpp"
#include "game/player_appearance.hpp"
#include "title/services/titles.hpp"
#include "pet/domain/records.hpp"
#include "pet/services/pet.hpp"
#include "game/emote_table.hpp"
#include "core/event_bus.hpp"
#include "game/game_events.hpp"
#include "core/env_flag.hpp"
#include "game/game_handler.hpp"
#include "game/protocol_constants.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "game/chat_handler.hpp"
#include "game/transport_manager.hpp"
#include "game/movement_handler.hpp"
#include "game/inventory_handler.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "network/world_connection.hpp"
#include <algorithm>
#include <bit>
#include <set>
#include <cmath>
#include <cstring>
#include <zlib.h>

namespace wowee {
namespace game {

namespace {


float slowUpdateObjectBlockLogThresholdMs() {
    static const int thresholdMs =
        core::envIntClamped("WOWEE_NET_SLOW_UPDATE_BLOCK_LOG_MS", 10, 1, 60000);
    return static_cast<float>(thresholdMs);
}

// A unit's speeds as its movement block carries them. A speed an expansion
// does not send arrives as zero and keeps what the unit had.
void applyBlockMovementSpeeds(const UpdateBlock& block, Entity& entity) {
    if (!block.hasMovement || !entity.isUnit()) return;
    auto& unit = static_cast<Unit&>(entity);
    const auto take = [&unit](float MovementSpeeds::* which, float sent) {
        if (std::isfinite(sent) && sent > 0.01f && sent < 200.0f) unit.setMovementSpeed(which, sent);
    };
    take(&MovementSpeeds::walk, block.walkSpeed);
    take(&MovementSpeeds::run, block.runSpeed);
    take(&MovementSpeeds::runBack, block.runBackSpeed);
    take(&MovementSpeeds::swim, block.swimSpeed);
    take(&MovementSpeeds::swimBack, block.swimBackSpeed);
    take(&MovementSpeeds::flight, block.flightSpeed);
    take(&MovementSpeeds::flightBack, block.flightBackSpeed);
    take(&MovementSpeeds::turnRate, block.turnRate);
}

} // anonymous namespace

WorldHandler::WorldHandler(GameHandler& owner)
    : owner_(owner) { initTypeHandlers(); }

void WorldHandler::registerOpcodes(DispatchTable& table) {
    // World object updates - accept during ENTERING_WORLD too so that entity
    // packets arriving before SMSG_LOGIN_VERIFY_WORLD are parsed and queued
    // rather than silently dropped (the budget system processes them later once
    // the state transitions to IN_WORLD).
    auto inWorldOrEntering = [this]() {
        auto s = owner_.getState();
        return s == WorldState::IN_WORLD || s == WorldState::ENTERING_WORLD;
    };
    table[Opcode::SMSG_UPDATE_OBJECT] = [this, inWorldOrEntering](network::Packet& packet) {
        LOG_DEBUG("Received SMSG_UPDATE_OBJECT, state=", static_cast<int>(owner_.getState()), " size=", packet.getSize());
        if (inWorldOrEntering()) handleUpdateObject(packet);
    };
    table[Opcode::SMSG_COMPRESSED_UPDATE_OBJECT] = [this, inWorldOrEntering](network::Packet& packet) {
        LOG_DEBUG("Received SMSG_COMPRESSED_UPDATE_OBJECT, state=", static_cast<int>(owner_.getState()), " size=", packet.getSize());
        if (inWorldOrEntering()) handleCompressedUpdateObject(packet);
    };
    table[Opcode::SMSG_DESTROY_OBJECT] = [this, inWorldOrEntering](network::Packet& packet) {
        if (inWorldOrEntering()) handleDestroyObject(packet);
    };

}

void WorldHandler::clearAll() {
    pendingUpdateObjectWork_.clear();
    transportGuids_.clear();
    serverUpdatedTransportGuids_.clear();
    entityManager.clear();
}

// ============================================================
// Update Object Pipeline
// ============================================================

void WorldHandler::enqueueUpdateObjectWork(UpdateObjectData&& data) {
    pendingUpdateObjectWork_.push_back(PendingUpdateObjectWork{.data = std::move(data)});
}
void WorldHandler::processPendingUpdateObjectWork(const std::chrono::steady_clock::time_point& start,
                                                 float budgetMs) {
    if (pendingUpdateObjectWork_.empty()) {
        return;
    }

    // Bounded by the caller's time budget alone. A count of 24 blocks a frame
    // used to sit beside it, and was what held a world-entry burst back for
    // seconds; spawning what the blocks create is queued separately anyway.
    while (!pendingUpdateObjectWork_.empty()) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= budgetMs) {
            break;
        }

        auto& work = pendingUpdateObjectWork_.front();
        if (!work.outOfRangeProcessed) {
            // Within the frame's budget, resuming next frame. A batch of
            // twenty-odd creatures leaving range all at once cost 20-32 ms of
            // one frame. The blocks of this same update wait until every
            // removal is done, so a guid that leaves and is created again in
            // one packet still goes out before it comes back in.
            const auto deadline = start + std::chrono::microseconds(
                static_cast<int64_t>(budgetMs * 1000.0f));
            static float costs[4] = {0, 0, 0, 0};
            if (work.nextOutOfRangeIndex == 0) { for (float& c : costs) c = 0.0f; }
            const auto outOfRangeStart = std::chrono::steady_clock::now();
            work.nextOutOfRangeIndex = processOutOfRangeObjectsUntil(
                work.data.outOfRangeGuids, work.nextOutOfRangeIndex, deadline, costs);
            work.outOfRangeMs += std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - outOfRangeStart).count();
            if (work.nextOutOfRangeIndex < work.data.outOfRangeGuids.size()) break;  // next frame
            if (work.outOfRangeMs > slowUpdateObjectBlockLogThresholdMs()) {
                LOG_WARNING("SLOW update-object out-of-range handling: ", work.outOfRangeMs,
                            "ms guidCount=", work.data.outOfRangeGuids.size(),
                            " (creatures ", costs[0], "ms, players ", costs[1],
                            "ms, game objects ", costs[2], "ms, rest ", costs[3], "ms)");
            }
            work.outOfRangeProcessed = true;
        }

        while (work.nextBlockIndex < work.data.blocks.size()) {
            elapsedMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsedMs >= budgetMs) {
                break;
            }

            const UpdateBlock& block = work.data.blocks[work.nextBlockIndex];
            auto blockStart = std::chrono::steady_clock::now();
            applyUpdateObjectBlock(block, work.newItemCreated);
            float blockMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - blockStart).count();
            if (blockMs > slowUpdateObjectBlockLogThresholdMs()) {
                LOG_WARNING("SLOW update-object block apply: ", blockMs,
                            "ms index=", work.nextBlockIndex,
                            " type=", static_cast<int>(block.updateType),
                            " guid=0x", std::hex, block.guid, std::dec,
                            " objectType=", static_cast<int>(block.objectType),
                            " fieldCount=", block.fields.size(),
                            " hasMovement=", block.hasMovement ? 1 : 0);
            }
            ++work.nextBlockIndex;
        }

        if (work.nextBlockIndex >= work.data.blocks.size()) {
            finalizeUpdateObjectBatch(work.newItemCreated);
            pendingUpdateObjectWork_.pop_front();
            continue;
        }
        break;
    }

    if (!pendingUpdateObjectWork_.empty()) {
        const auto& work = pendingUpdateObjectWork_.front();
        LOG_DEBUG("GameHandler update-object budget reached (remainingBatches=",
                  pendingUpdateObjectWork_.size(), ", nextBlockIndex=", work.nextBlockIndex,
                  "/", work.data.blocks.size(), ", owner_.getState()=", worldStateName(owner_.getState()), ")");
    }
}
void WorldHandler::handleUpdateObject(network::Packet& packet) {
    UpdateObjectData data;
    if (!owner_.getPacketParsers()->parseUpdateObject(packet, data)) {
        static int updateObjErrors = 0;
        if (++updateObjErrors <= 5)
            LOG_WARNING("Failed to parse SMSG_UPDATE_OBJECT");
        if (data.blocks.empty()) return;
        // Fall through: process any blocks that were successfully parsed before the failure.
    }

    enqueueUpdateObjectWork(std::move(data));
}

size_t WorldHandler::processOutOfRangeObjectsUntil(const std::vector<uint64_t>& guids, size_t next,
                                                       std::chrono::steady_clock::time_point deadline,
                                                       float costs[4]) {
    using clock = std::chrono::steady_clock;
    auto since = [](clock::time_point t) {
        return std::chrono::duration<float, std::milli>(clock::now() - t).count();
    };
    bool inventoryChanged = false;
    for (; next < guids.size(); ++next) {
        if (next > 0 && clock::now() >= deadline) break;
        const uint64_t guid = guids[next];
        const auto objectStart = clock::now();
        // An item leaving is how the server says it is gone. Selling one, or
        // handing it to a quest, takes it out of range rather than destroying
        // it, and handleDestroyObject is the only path that was clearing the
        // online item tracking - so a sold item stayed in the picture the bags
        // are drawn from and went on being drawn in the slot it left.
        //
        // Before the entity check, because an item need not be in the entity
        // manager at all: the partial updates that carry them are tracked in
        // onlineItems_ alone, which is why the values path has a branch for
        // exactly that case.
        if (owner_.inventory().forgetItemObject(guid)) inventoryChanged = true;

        auto entity = entityManager.getEntity(guid);
        if (!entity) continue;

        const bool isKnownTransport = transportGuids_.count(guid) > 0;
        if (isKnownTransport) {
            // Keep transports alive across out-of-range flapping.
            // Boats/zeppelins are global movers and removing them here can make
            // them disappear until a later movement snapshot happens to recreate them.
            const bool playerAboardNow = (owner_.movement().getPlayerTransportGuid() == guid);
            const bool stickyAboard = owner_.movement().recentlyAboard(guid);
            const bool movementSaysAboard = (owner_.movementInfoRef().transportGuid == guid);
            LOG_INFO("Preserving transport on out-of-range: 0x",
                     std::hex, guid, std::dec,
                     " now=", playerAboardNow,
                     " sticky=", stickyAboard,
                     " movement=", movementSaysAboard);
            continue;
        }

        LOG_DEBUG("Entity went out of range: 0x", std::hex, guid, std::dec);
        // Trigger despawn callbacks before removing entity
        const ObjectType type = entity->getType();
        const int kind = type == ObjectType::UNIT ? 0 : type == ObjectType::PLAYER ? 1
                       : type == ObjectType::GAMEOBJECT ? 2 : 3;
        if (entity->getType() == ObjectType::UNIT) {
            owner_.eventBus().publish(events::CreatureDespawned{guid});
        } else if (entity->getType() == ObjectType::PLAYER) {
            owner_.eventBus().publish(events::PlayerDespawned{guid});
            // What they wore is decoded again if they come back: forgotten
            // whether or not anything draws them.
            owner_.inventory().forgetPlayerEquipment(guid);
            owner_.queryCache().playerLeftView(guid);
        } else if (entity->getType() == ObjectType::GAMEOBJECT) {
            owner_.eventBus().publish(events::GameObjectDespawned{guid});
        } else if (entity->getType() == ObjectType::CORPSE) {
            owner_.eventBus().publish(events::PlayerDespawned{guid});
        }
        transportGuids_.erase(guid);
        serverUpdatedTransportGuids_.erase(guid);
        owner_.movement().clearTransportAttachment(guid);
        if (owner_.movement().getPlayerTransportGuid() == guid) {
            owner_.movement().clearPlayerTransport();
        }
        entity.reset();
        entityManager.removeEntity(guid);
        costs[kind] += since(objectStart);
    }

    // Once per batch, not per item: a vendor sale can take several items out
    // of range together, and rebuilding per item would redraw the bags as many
    // times for one answer.
    if (inventoryChanged) owner_.inventory().rebuildOnlineInventory();
    return next;
}

// ============================================================
// Extracted helper methods
// ============================================================

std::optional<PlayerAppearance> WorldHandler::appearanceOf(uint64_t guid) const {
    auto entity = entityManager.getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) return std::nullopt;
    return playerAppearance(entity->getFields());
}

void WorldHandler::applyUpdateObjectBlock(const UpdateBlock& block, bool& newItemCreated) {
    switch (block.updateType) {
        case UpdateType::CREATE_OBJECT:
        case UpdateType::CREATE_OBJECT2:
            handleCreateObject(block, newItemCreated);
            break;
        case UpdateType::VALUES:
            handleValuesUpdate(block);
            break;
        case UpdateType::MOVEMENT:
            handleMovementUpdate(block);
            break;
        default:
            break;
    }
}

// ============================================================
// Concern-specific helpers
// ============================================================

// Non-player transport child attachment - identical in CREATE/VALUES/MOVEMENT
void WorldHandler::updateNonPlayerTransportAttachment(const UpdateBlock& block,
                                                           const std::shared_ptr<Entity>& entity,
                                                           ObjectType entityType) {
    if (block.guid == owner_.getPlayerGuid()) return;
    if (entityType != ObjectType::UNIT && entityType != ObjectType::GAMEOBJECT) return;

    if (block.onTransport && block.transportGuid != 0) {
        const glm::vec3 serverOffset(block.transportX, block.transportY, block.transportZ);
        const bool transportResolved = owner_.getTransportManager() &&
            owner_.getTransportManager()->getTransport(block.transportGuid);
        // Preserve the raw wire offset if the child arrives before its parent
        // transport during a map load. Its coordinate convention depends on
        // whether that parent is an M2 or WMO and can only be decided once the
        // transport has registered.
        glm::vec3 localOffset = transportResolved
            ? owner_.getTransportManager()->serverToTransportLocal(block.transportGuid, serverOffset)
            : serverOffset;
        const bool hasLocalOrientation = (block.updateFlags & 0x0020) != 0; // UPDATEFLAG_LIVING
        float localOriCanonical = core::coords::normalizeAngleRad(-block.transportO);
        owner_.movement().setTransportAttachment(block.guid, entityType, block.transportGuid,
                               localOffset, hasLocalOrientation, localOriCanonical,
                               !transportResolved);
        if (owner_.getTransportManager() && owner_.getTransportManager()->getTransport(block.transportGuid)) {
            glm::vec3 composed = owner_.getTransportManager()->getPlayerWorldPosition(block.transportGuid, localOffset);
            entity->setPosition(composed.x, composed.y, composed.z, entity->getOrientation());
        }
    } else {
        owner_.movement().clearTransportAttachment(block.guid);
    }
}

namespace {
/// A float field arrives as the raw uint32 its bits spell. Named once rather
/// than written out at each of the scale fields -- which two of them still
/// did, in memcpy form, until this was pointed at them.
float bitsToFloat(uint32_t raw) {
    return std::bit_cast<float>(raw);
}
}  // namespace

// Resolve cached field indices once per handler call.
WorldHandler::UnitFieldIndices WorldHandler::UnitFieldIndices::resolve() {
    return UnitFieldIndices{
        .health = fieldIndex(UF::UNIT_FIELD_HEALTH),
        .maxHealth = fieldIndex(UF::UNIT_FIELD_MAXHEALTH),
        .powerBase = fieldIndex(UF::UNIT_FIELD_POWER1),
        .maxPowerBase = fieldIndex(UF::UNIT_FIELD_MAXPOWER1),
        .level = fieldIndex(UF::UNIT_FIELD_LEVEL),
        .faction = fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE),
        .flags = fieldIndex(UF::UNIT_FIELD_FLAGS),
        .dynFlags = fieldIndex(UF::UNIT_DYNAMIC_FLAGS),
        .auraState = fieldIndex(UF::UNIT_FIELD_AURASTATE),
        .displayId = fieldIndex(UF::UNIT_FIELD_DISPLAYID),
        .nativeDisplayId = fieldIndex(UF::UNIT_FIELD_NATIVEDISPLAYID),
        .mountDisplayId = fieldIndex(UF::UNIT_FIELD_MOUNTDISPLAYID),
        .npcFlags = fieldIndex(UF::UNIT_NPC_FLAGS),
        .npcEmoteState = fieldIndex(UF::UNIT_NPC_EMOTESTATE),
        // In the struct's own order: this is an aggregate initializer, so a
        // pair added in the wrong place here silently assigns two other fields.
        .boundingRadius = fieldIndex(UF::UNIT_FIELD_BOUNDINGRADIUS),
        .combatReach = fieldIndex(UF::UNIT_FIELD_COMBATREACH),
        .bytes0 = fieldIndex(UF::UNIT_FIELD_BYTES_0),
        .bytes1 = fieldIndex(UF::UNIT_FIELD_BYTES_1),
        .petXp = fieldIndex(UF::UNIT_FIELD_PETEXPERIENCE),
        .petNextLevelXp = fieldIndex(UF::UNIT_FIELD_PETNEXTLEVELEXP),
        .stat0 = fieldIndex(UF::UNIT_FIELD_STAT0),
        .resistances = fieldIndex(UF::UNIT_FIELD_RESISTANCES),
        .attackPower = fieldIndex(UF::UNIT_FIELD_ATTACK_POWER),
        .minDamage = fieldIndex(UF::UNIT_FIELD_MINDAMAGE),
        .maxDamage = fieldIndex(UF::UNIT_FIELD_MAXDAMAGE)
    };
}

// Create the appropriate Entity subclass from the block's object type.
std::shared_ptr<Entity> WorldHandler::createEntityFromBlock(const UpdateBlock& block) {
    switch (block.objectType) {
        case ObjectType::PLAYER:
            return std::make_shared<Player>(block.guid);
        case ObjectType::UNIT:
            return std::make_shared<Unit>(block.guid);
        case ObjectType::GAMEOBJECT:
            return std::make_shared<GameObject>(block.guid);
        default: {
            auto entity = std::make_shared<Entity>(block.guid);
            entity->setType(block.objectType);
            return entity;
        }
    }
}

//     Apply unit fields during CREATE - sets health/power/level/flags/displayId/etc.
//     Returns true if the entity is initially dead (health=0 or DYNFLAG_DEAD).
bool WorldHandler::applyUnitFieldsOnCreate(const UpdateBlock& block,
                                                 std::shared_ptr<Unit>& unit,
                                                 const UnitFieldIndices& ufi) {
    bool unitInitiallyDead = false;
    for (const auto& [key, val] : block.fields) {
        // Check all specific fields BEFORE power/maxpower range checks.
        // In Classic, power indices (23-27) are adjacent to maxHealth (28),
        // and maxPower indices (29-33) are adjacent to level (34) and faction (35).
        // A range check like "key >= powerBase && key < powerBase+7" would
        // incorrectly capture maxHealth/level/faction in Classic's tight layout.
        // The events FrameXML's unit frames update on. Each carries the unit id
        // as its first argument, which is how a frame knows whether the change
        // was to the unit it is showing.
        auto emitForUnit = [&](const char* event) {
            if (!owner_.hasAddonEventListener()) return;
            const auto uid = owner_.guidToUnitId(block.guid);
            if (!uid.empty()) pendingEvents_.emit(event, {uid});
        };

        if (key == ufi.health) {
            unit->setHealth(val);
            emitForUnit("UNIT_HEALTH");
            if ((block.objectType == ObjectType::UNIT ||
                 block.objectType == ObjectType::PLAYER) && val == 0) {
                unitInitiallyDead = true;
            }
        } else if (key == ufi.maxHealth) {
            unit->setMaxHealth(val);
            emitForUnit("UNIT_MAXHEALTH");
        }
        else if (key == ufi.level) {
            unit->setLevel(val);
            emitForUnit("UNIT_LEVEL");
        } else if (key == ufi.faction) {
            unit->setFactionTemplate(val);
            if (owner_.hasAddonEventListener()) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_FACTION", {uid});
            }
        }
        else if (key == ufi.flags) {
            unit->setUnitFlags(val);
            if (owner_.hasAddonEventListener()) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_FLAGS", {uid});
            }
        }
        else if (ufi.auraState != 0xFFFF && key == ufi.auraState) {
            unit->setAuraState(val);
        }
        else if (key == ufi.bytes0) {
            unit->setPowerType(static_cast<uint8_t>((val >> 24) & 0xFF));
            // Which bar to show at all - a druid shifting form changes it.
            emitForUnit("UNIT_DISPLAYPOWER");
        } else if (key == ufi.boundingRadius) {
            unit->setBoundingRadius(bitsToFloat(val));
        } else if (key == ufi.combatReach) {
            unit->setCombatReach(bitsToFloat(val));
        } else if (key == ufi.displayId) {
            unit->setDisplayId(val);
            if (owner_.hasAddonEventListener()) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty()) {
                    pendingEvents_.emit("UNIT_MODEL_CHANGED", {uid});
                    // The portrait is drawn from the display id, so a unit that
                    // changes model - a shapeshift, a polymorph, a mount -
                    // needs its portrait redrawn too. Six frames listen for
                    // this and none of them had ever heard it.
                    pendingEvents_.emit("UNIT_PORTRAIT_UPDATE", {uid});
                }
            }
        }
        else if (key == ufi.nativeDisplayId) { unit->setNativeDisplayId(val); }
        else if (key == ufi.npcFlags) { unit->setNpcFlags(val); }
        else if (key == ufi.npcEmoteState) { unit->setNpcEmoteState(val); }
        else if (key == ufi.dynFlags) {
            unit->setDynamicFlags(val);
            if ((block.objectType == ObjectType::UNIT ||
                 block.objectType == ObjectType::PLAYER) &&
                ((val & UNIT_DYNFLAG_DEAD) != 0 || (val & UNIT_DYNFLAG_LOOTABLE) != 0)) {
                unitInitiallyDead = true;
            }
        }
        // Power/maxpower range checks AFTER all specific fields
        else if (key >= ufi.powerBase && key < ufi.powerBase + 7) {
            const auto powerType = static_cast<uint8_t>(key - ufi.powerBase);
            unit->setPowerByType(powerType, val);
            // Named per power rather than one event: FrameXML registers only
            // the one its bar shows, so a rogue's frame is not woken by every
            // mana tick in the party.
            static const char* kPowerEvents[7] = {
                "UNIT_MANA", "UNIT_RAGE", "UNIT_FOCUS", "UNIT_ENERGY",
                "UNIT_HAPPINESS", "UNIT_RUNIC_POWER", "UNIT_RUNIC_POWER"
            };
            if (powerType < 7) emitForUnit(kPowerEvents[powerType]);
        } else if (key >= ufi.maxPowerBase && key < ufi.maxPowerBase + 7) {
            unit->setMaxPowerByType(static_cast<uint8_t>(key - ufi.maxPowerBase), val);
            // The maximum a bar is scaled against, which FrameXML redraws on.
            static const char* kMaxPowerEvents[7] = {
                "UNIT_MAXMANA", "UNIT_MAXRAGE", "UNIT_MAXFOCUS", "UNIT_MAXENERGY",
                "UNIT_MAXHAPPINESS", "UNIT_MAXRUNIC_POWER", "UNIT_MAXRUNIC_POWER"
            };
            if (const auto t = static_cast<uint8_t>(key - ufi.maxPowerBase); t < 7)
                emitForUnit(kMaxPowerEvents[t]);
        }
        else if (key == ufi.mountDisplayId) {
            if (block.guid == owner_.getPlayerGuid()) {
                owner_.movement().playerMountFieldArrived(val, block.fields, pendingEvents_);
            } else if (block.objectType == ObjectType::PLAYER) {
                owner_.eventBus().publish(events::OtherPlayerMounted{block.guid, val});
            }
            unit->setMountDisplayId(val);
        }
    }
    // Initial update masks commonly omit fields whose value is zero. A dead unit
    // can therefore arrive without UNIT_FIELD_HEALTH even though its default
    // client-side health is zero; the dynamic corpse bits remain authoritative.
    if ((block.objectType == ObjectType::UNIT || block.objectType == ObjectType::PLAYER) &&
        isUnitCorpseState(unit->getHealth(), unit->getMaxHealth(), unit->getDynamicFlags())) {
        unitInitiallyDead = true;
    }
    return unitInitiallyDead;
}

// 3c: Apply unit fields during VALUES update - tracks health/power/display changes
//     and fires events for transitions (death, resurrect, level up, etc.).
WorldHandler::UnitFieldUpdateResult WorldHandler::applyUnitFieldsOnUpdate(
        const UpdateBlock& block, const std::shared_ptr<Entity>& entity,
        std::shared_ptr<Unit>& unit, const UnitFieldIndices& ufi) {
    UnitFieldUpdateResult result;
    result.oldDisplayId = unit->getDisplayId();
    uint32_t oldHealth = unit->getHealth();
    // The pet's own fields from this update, handed over together below.
    pet::PetFields petFields;
    bool petFieldsArrived = false;
    const uint64_t petGuid = owner_.petGuid();
    for (const auto& [key, val] : block.fields) {
        if (key == ufi.health) {
            unit->setHealth(val);
            result.healthChanged = true;
            owner_.combat().unitHealthArrived(block.guid, oldHealth, val, pendingEvents_);
            if (val == 0) {
                if (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) {
                    owner_.eventBus().publish(events::UnitDied{block.guid});
                    result.npcDeathNotified = true;
                }
            } else if (oldHealth == 0 && val > 0) {
                if (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) {
                    owner_.eventBus().publish(events::UnitRespawned{block.guid});
                    result.npcRespawnNotified = true;
                }
            }
        // Specific fields checked BEFORE power/maxpower range checks
        // (Classic packs maxHealth/level/faction adjacent to power indices)
        } else if (key == ufi.maxHealth) {
            unit->setMaxHealth(val);
            result.healthChanged = true;
            result.maxHealthChanged = true;
        }
        else if (key == ufi.bytes0) {
            uint8_t oldPT = unit->getPowerType();
            unit->setPowerType(static_cast<uint8_t>((val >> 24) & 0xFF));
            if (unit->getPowerType() != oldPT) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_DISPLAYPOWER", {uid});
            }
        } else if (key == ufi.flags) {
            uint32_t oldFlags = unit->getUnitFlags();
            unit->setUnitFlags(val);
            if (block.guid == owner_.getPlayerGuid()) {
                owner_.combat().playerUnitFlagsArrived(oldFlags, val, pendingEvents_);
                owner_.player().unitFlagsArrived(oldFlags, val);
            }
        }
        else if (ufi.auraState != 0xFFFF && key == ufi.auraState) {
            unit->setAuraState(val);
        }
        else if (ufi.bytes1 != 0xFFFF && key == ufi.bytes1) {
            const uint8_t oldVisibilityFlags = unit->getVisibilityFlags();
            const uint8_t newVisibilityFlags = static_cast<uint8_t>((val >> 16) & 0xFF);
            unit->setVisibilityFlags(newVisibilityFlags);

            if (block.guid == owner_.getPlayerGuid()) {
                owner_.player().visibilityFlagsArrived(oldVisibilityFlags, newVisibilityFlags);
                owner_.player().shapeshiftFormArrived(static_cast<uint8_t>((val >> 24) & 0xFF),
                                                      pendingEvents_);
            }
        }
        else if (key == ufi.dynFlags) {
            uint32_t oldDyn = unit->getDynamicFlags();
            unit->setDynamicFlags(val);
            if (block.guid == owner_.getPlayerGuid()) {
                owner_.combat().playerDynamicFlagsArrived(oldDyn, val, pendingEvents_);
            } else if (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) {
                bool wasDead = (oldDyn & UNIT_DYNFLAG_DEAD) != 0;
                bool nowDead = (val & UNIT_DYNFLAG_DEAD) != 0;
                if (!wasDead && nowDead) {
                    if (!result.npcDeathNotified) {
                        owner_.eventBus().publish(events::UnitDied{block.guid});
                        result.npcDeathNotified = true;
                    }
                } else if (wasDead && !nowDead) {
                    if (!result.npcRespawnNotified) {
                        owner_.eventBus().publish(events::UnitRespawned{block.guid});
                        result.npcRespawnNotified = true;
                    }
                }
                if (entity->getType() == ObjectType::UNIT &&
                    (oldDyn & UNIT_DYNFLAG_LOOTABLE) != 0 &&
                    (val & UNIT_DYNFLAG_LOOTABLE) == 0) {
                    result.lootableCleared = true;
                }
            }
        } else if (key == ufi.level) {
            uint32_t oldLvl = unit->getLevel();
            unit->setLevel(val);
            if (val != oldLvl) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_LEVEL", {uid});
            }
            if (block.guid != owner_.getPlayerGuid() &&
                entity->getType() == ObjectType::PLAYER &&
                val > oldLvl && oldLvl > 0) {
                owner_.eventBus().publish(events::OtherPlayerLevelUp{block.guid, val});
            }
        }
        else if (key == ufi.faction) {
            unit->setFactionTemplate(val);
            unit->setHostile(owner_.isHostileFaction(val));
        } else if (key == ufi.boundingRadius) {
            unit->setBoundingRadius(bitsToFloat(val));
        } else if (key == ufi.combatReach) {
            unit->setCombatReach(bitsToFloat(val));
        } else if (key == ufi.displayId) {
            if (val != unit->getDisplayId()) {
                unit->setDisplayId(val);
                result.displayIdChanged = true;
            }
        } else if (key == ufi.nativeDisplayId) {
            unit->setNativeDisplayId(val);
        } else if (key == ufi.mountDisplayId) {
            if (block.guid == owner_.getPlayerGuid()) {
                owner_.movement().playerMountFieldArrived(val, block.fields, pendingEvents_);
            } else if (entity->getType() == ObjectType::PLAYER &&
                       val != unit->getMountDisplayId()) {
                owner_.eventBus().publish(events::OtherPlayerMounted{block.guid, val});
            }
            unit->setMountDisplayId(val);
        } else if (ufi.attackPower != 0xFFFF && key == ufi.attackPower &&
                   block.guid == petGuid) {
            petFields.attackPower = static_cast<int32_t>(val);
            petFieldsArrived = true;
        } else if (ufi.minDamage != 0xFFFF && key == ufi.minDamage &&
                   block.guid == petGuid) {
            // A float, sent as its bits like every other float field.
            float damage = 0.0f;
            std::memcpy(&damage, &val, 4);
            petFields.minDamage = damage;
            petFieldsArrived = true;
        } else if (ufi.maxDamage != 0xFFFF && key == ufi.maxDamage &&
                   block.guid == petGuid) {
            float damage = 0.0f;
            std::memcpy(&damage, &val, 4);
            petFields.maxDamage = damage;
            petFieldsArrived = true;
        } else if (ufi.stat0 != 0xFFFF && key >= ufi.stat0 && key < ufi.stat0 + 5 &&
                   block.guid == petGuid) {
            // The pet's own five, not the player's. The paperdoll's pet tab
            // reads them through UnitStat("pet"), which used to answer from the
            // player - so a hunter's pet listed its owner's Strength.
            petFields.stats[key - ufi.stat0] = static_cast<int32_t>(val);
            petFieldsArrived = true;
        } else if (ufi.resistances != 0xFFFF && key >= ufi.resistances &&
                   key < ufi.resistances + 7 && block.guid == petGuid) {
            // Armor is index zero and the six schools follow it, the same shape
            // as the player's.
            petFields.resistances[key - ufi.resistances] = static_cast<int32_t>(val);
            petFieldsArrived = true;
        } else if (ufi.petXp != 0xFFFF && key == ufi.petXp &&
                   block.guid == petGuid) {
            // Only the player's own pet. Every unit carries these fields and
            // only one of them has an experience bar to draw.
            petFields.experience = val;
            petFieldsArrived = true;
        } else if (ufi.petNextLevelXp != 0xFFFF && key == ufi.petNextLevelXp &&
                   block.guid == petGuid) {
            petFields.nextLevelExperience = val;
            petFieldsArrived = true;
        } else if (key == ufi.npcFlags) { unit->setNpcFlags(val); }
        else if (key == ufi.npcEmoteState) {
            uint32_t oldEmote = unit->getNpcEmoteState();
            unit->setNpcEmoteState(val);
            // Fire emote animation callback so entity_spawner can update the NPC's idle anim
            if (val != oldEmote) {
                uint32_t animId = val != 0 ? owner_.emotes().animByEmotesId(val) : 0;
                if (val == 0 || animId != 0) {
                    // UNIT_NPC_EMOTESTATE is persistent by definition - a zero
                    // here genuinely clears the state loop.
                    owner_.eventBus().publish(events::UnitEmotePlayed{block.guid, animId, true});
                } else {
                    LOG_DEBUG("UNIT_NPC_EMOTESTATE emoteId=", val, " had no Emotes.dbc animation mapping");
                }
            }
        }
        // Power/maxpower range checks AFTER all specific fields
        else if (key >= ufi.powerBase && key < ufi.powerBase + 7) {
            unit->setPowerByType(static_cast<uint8_t>(key - ufi.powerBase), val);
            result.powerChanged = true;
            result.powerTypeChanged = static_cast<int>(key - ufi.powerBase);
        } else if (key >= ufi.maxPowerBase && key < ufi.maxPowerBase + 7) {
            unit->setMaxPowerByType(static_cast<uint8_t>(key - ufi.maxPowerBase), val);
            result.powerChanged = true;
            result.maxPowerTypeChanged = static_cast<int>(key - ufi.maxPowerBase);
        }
    }

    // Fire UNIT_HEALTH / UNIT_POWER events for Lua addons
    if ((result.healthChanged || result.powerChanged)) {
        auto unitId = owner_.guidToUnitId(block.guid);
        if (!unitId.empty()) {
            if (result.healthChanged) pendingEvents_.emit("UNIT_HEALTH", {unitId});
            if (result.maxHealthChanged) pendingEvents_.emit("UNIT_MAXHEALTH", {unitId});
            if (result.powerChanged) {
                // The event a WotLK interface is listening for is named after
                // the power itself. UNIT_POWER is the later, generic one - it
                // arrived in Cataclysm, and every unit frame this client
                // targets registers UNIT_MANA, UNIT_RAGE, UNIT_ENERGY or
                // UNIT_FOCUS instead. Sending only the generic name meant the
                // mana bar was told nothing it understood and never moved,
                // even though the number behind it was current.
                //
                // The create path already names them; this is the same table.
                static const char* kPowerEvents[7] = {
                    "UNIT_MANA", "UNIT_RAGE", "UNIT_FOCUS", "UNIT_ENERGY",
                    "UNIT_HAPPINESS", "UNIT_RUNIC_POWER", "UNIT_RUNIC_POWER"
                };
                static const char* kMaxPowerEvents[7] = {
                    "UNIT_MAXMANA", "UNIT_MAXRAGE", "UNIT_MAXFOCUS", "UNIT_MAXENERGY",
                    "UNIT_MAXHAPPINESS", "UNIT_MAXRUNIC_POWER", "UNIT_MAXRUNIC_POWER"
                };
                if (result.powerTypeChanged >= 0 && result.powerTypeChanged < 7)
                    pendingEvents_.emit(kPowerEvents[result.powerTypeChanged], {unitId});
                if (result.maxPowerTypeChanged >= 0 && result.maxPowerTypeChanged < 7)
                    pendingEvents_.emit(kMaxPowerEvents[result.maxPowerTypeChanged], {unitId});
                pendingEvents_.emit("UNIT_POWER", {unitId});
                // When player power changes, action bar usability may change
                if (block.guid == owner_.getPlayerGuid()) {
                    pendingEvents_.emit("ACTIONBAR_UPDATE_USABLE", {});
                    pendingEvents_.emit("SPELL_UPDATE_USABLE", {});
                }
            }
        }
    }

    // For the wounded-idle animation.
    if (result.healthChanged && block.guid == owner_.getPlayerGuid()) {
        owner_.eventBus().publish(events::PlayerHealthChanged{unit->getHealth(), unit->getMaxHealth()});
    }

    // Once per update, so the pet frame and the pet tab redraw once for it.
    if (petFieldsArrived) {
        if (auto* feed = owner_.petFeed()) feed->onFields(petFields);
    }

    return result;
}

//     Apply player stat fields (XP, coinage, combat stats, etc.).
//     Shared between CREATE and VALUES - isCreate controls event firing differences.
//     Dispatch entity spawn callbacks for units/players.
//     Consolidates player/creature spawn callback invocation from CREATE and VALUES handlers.
//     isDead = unitInitiallyDead (CREATE) or computed isDeadNow && !npcDeathNotified (VALUES).
void WorldHandler::dispatchEntitySpawn(uint64_t guid, ObjectType objectType,
                                            const std::shared_ptr<Entity>& entity,
                                            const std::shared_ptr<Unit>& unit,
                                            bool isDead) {
    announceUnit(guid, objectType, entity, unit, isDead);
    // Query quest giver status for NPCs with questgiver flag (0x02)
    if (objectType == ObjectType::UNIT && (unit->getNpcFlags() & NPC_FLAG_QUESTGIVER) &&
        owner_.getSocket()) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(guid);
        owner_.getSocket()->send(qsPkt);
    }
}

void WorldHandler::announceUnit(uint64_t guid, ObjectType objectType,
                                     const std::shared_ptr<Entity>& entity,
                                     const std::shared_ptr<Unit>& unit,
                                     bool isDead) {
    if (objectType == ObjectType::PLAYER && guid == owner_.getPlayerGuid()) {
        return;  // Skip local player - spawned separately via spawnPlayerCharacter()
    }
    if (objectType == ObjectType::PLAYER) {
        if (const auto look = playerAppearance(entity->getFields())) {
            owner_.eventBus().publish(events::PlayerSpawned{
                guid, unit->getDisplayId(), look->race, look->gender, look->appearanceBytes,
                look->facialFeatures, unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation()});
        } else {
            LOG_WARNING("[Spawn] PLAYER guid=0x", std::hex, guid, std::dec,
                      " displayId=", unit->getDisplayId(), " has no UNIT_FIELD_BYTES_0 - no race, model will not render");
        }
    } else {
        LOG_DEBUG("[Spawn] UNIT guid=0x", std::hex, guid, std::dec,
                  " displayId=", unit->getDisplayId(), " at (",
                  unit->getX(), ",", unit->getY(), ",", unit->getZ(), ")");
        float unitScale = 1.0f;
        uint16_t scaleIdx = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
        if (scaleIdx != 0xFFFF) {
            // raw == 0 means the field was never populated (IEEE 754 0.0f is all-zero bits).
            // Keep the default 1.0f rather than setting scale to 0 and making the entity invisible.
            uint32_t raw = entity->getField(scaleIdx);
            if (raw != 0) {
                unitScale = bitsToFloat(raw);
                if (unitScale <= 0.01f || unitScale > 100.0f) unitScale = 1.0f;
            }
        }
        owner_.eventBus().publish(events::CreatureSpawned{guid, unit->getDisplayId(),
            unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation(), unitScale});
    }
    if (isDead) owner_.eventBus().publish(events::UnitDied{guid});
}

void WorldHandler::announceObjectsInView() {
    size_t units = 0, players = 0, objects = 0, corpses = 0;
    for (const auto& [guid, entity] : entityManager.getEntities()) {
        if (!entity) continue;
        switch (entity->getType()) {
        case ObjectType::UNIT:
        case ObjectType::PLAYER: {
            auto unit = std::static_pointer_cast<Unit>(entity);
            if (unit->getDisplayId() == 0) break;  // Never drawn; its create said so.
            const bool dead = isUnitCorpseState(unit->getHealth(), unit->getMaxHealth(),
                                                unit->getDynamicFlags());
            announceUnit(guid, entity->getType(), entity, unit, dead);
            if (entity->getType() == ObjectType::PLAYER && guid != owner_.getPlayerGuid()) {
                ++players;
                if (unit->getMountDisplayId() != 0)
                    owner_.eventBus().publish(events::OtherPlayerMounted{guid, unit->getMountDisplayId()});
            } else if (entity->getType() == ObjectType::UNIT) {
                ++units;
            }
            break;
        }
        case ObjectType::GAMEOBJECT:
            announceGameObject(guid, static_cast<const GameObject&>(*entity));
            ++objects;
            break;
        case ObjectType::CORPSE:
            announceCorpse(guid, *entity);
            ++corpses;
            break;
        default:
            break;
        }
    }
    // Other players' clothes come from their visible items, kept beside the
    // entities by the inventory domain.
    owner_.inventory().emitAllOtherPlayerEquipment();
    LOG_INFO("Presented objects in view: units=", units, " players=", players,
             " gameobjects=", objects, " corpses=", corpses);
}

void WorldHandler::applyRecordedObjects(const UpdateObjectData& data) {
    float costs[4]{};
    processOutOfRangeObjectsUntil(data.outOfRangeGuids, 0,
        std::chrono::steady_clock::time_point::max(), costs);
    bool itemsCreated = false;
    for (const auto& block : data.blocks) applyUpdateObjectBlock(block, itemsCreated);
    finalizeUpdateObjectBatch(itemsCreated);
}

// Track online item/container objects during CREATE.
void WorldHandler::trackItemOnCreate(const UpdateBlock& block, bool& newItemCreated) {
    // The inventory reads what an item object's fields say; this rebuilds the
    // bags once for the whole packet.
    if (owner_.inventory().trackItemObject(block.guid, block.objectType == ObjectType::CONTAINER,
                                           block.fields)) {
        newItemCreated = true;
    }
}

// Update item stack count / durability / enchants for existing items during VALUES.
void WorldHandler::updateItemOnValuesUpdate(const UpdateBlock& block,
                                                  const std::shared_ptr<Entity>& entity) {
    owner_.inventory().itemObjectChanged(
        block.guid, entity && entity->getType() == ObjectType::CONTAINER, block.fields, pendingEvents_);
}

// ============================================================
// Object-type handler struct definitions
// ============================================================

struct WorldHandler::UnitTypeHandler : WorldHandler::IObjectTypeHandler {
    WorldHandler& ctl_;
    explicit UnitTypeHandler(WorldHandler& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreateUnit(block, entity); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdateUnit(block, entity); }
};

struct WorldHandler::PlayerTypeHandler : WorldHandler::IObjectTypeHandler {
    WorldHandler& ctl_;
    explicit PlayerTypeHandler(WorldHandler& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreatePlayer(block, entity); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdatePlayer(block, entity); }
};

struct WorldHandler::GameObjectTypeHandler : WorldHandler::IObjectTypeHandler {
    WorldHandler& ctl_;
    explicit GameObjectTypeHandler(WorldHandler& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreateGameObject(block, entity); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdateGameObject(block, entity); }
};

struct WorldHandler::ItemTypeHandler : WorldHandler::IObjectTypeHandler {
    WorldHandler& ctl_;
    explicit ItemTypeHandler(WorldHandler& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& /*entity*/, bool& newItemCreated) override { ctl_.onCreateItem(block, newItemCreated); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdateItem(block, entity); }
};

struct WorldHandler::CorpseTypeHandler : WorldHandler::IObjectTypeHandler {
    WorldHandler& ctl_;
    explicit CorpseTypeHandler(WorldHandler& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreateCorpse(block, *entity); }
};

// ============================================================
// Handler registry infrastructure
// ============================================================

void WorldHandler::initTypeHandlers() {
    typeHandlers_[static_cast<uint8_t>(ObjectType::UNIT)] = std::make_unique<UnitTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::PLAYER)] = std::make_unique<PlayerTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::GAMEOBJECT)] = std::make_unique<GameObjectTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::ITEM)] = std::make_unique<ItemTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::CONTAINER)] = std::make_unique<ItemTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::CORPSE)] = std::make_unique<CorpseTypeHandler>(*this);
}

WorldHandler::IObjectTypeHandler* WorldHandler::getTypeHandler(ObjectType type) const {
    auto it = typeHandlers_.find(static_cast<uint8_t>(type));
    return it != typeHandlers_.end() ? it->second.get() : nullptr;
}

// ============================================================
// Deferred event bus flush
// ============================================================

void WorldHandler::flushPendingEvents() {
    for (const auto& [name, args] : pendingEvents_.events) {
        owner_.fireAddonEvent(name, args);
    }
    pendingEvents_.clear();
}

// ============================================================
// Type-specific CREATE handlers
// ============================================================

void WorldHandler::onCreateUnit(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    // Name query for creatures
    auto it = block.fields.find(fieldIndex(UF::OBJECT_FIELD_ENTRY));
    if (it != block.fields.end() && it->second != 0) {
        auto unit = std::static_pointer_cast<Unit>(entity);
        unit->setEntry(it->second);
        std::string cached = owner_.queryCache().getCachedCreatureName(it->second);
        if (!cached.empty()) {
            unit->setName(cached);
        }
        owner_.queryCache().queryCreatureInfo(it->second, block.guid);
    }

    // Unit fields
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    bool unitInitiallyDead = applyUnitFieldsOnCreate(block, unit, ufi);

    // Hostility
    unit->setHostile(owner_.isHostileFaction(unit->getFactionTemplate()));

    // Spawn dispatch
    if (unit->getDisplayId() == 0) {
        LOG_WARNING("[Spawn] UNIT guid=0x", std::hex, block.guid, std::dec,
                  " has displayId=0 - no spawn (entry=", unit->getEntry(),
                  " at ", unit->getX(), ",", unit->getY(), ",", unit->getZ(), ")");
    }
    if (unit->getDisplayId() != 0) {
        dispatchEntitySpawn(block.guid, block.objectType, entity, unit, unitInitiallyDead);
        if (block.hasMovement && block.moveFlags != 0 && block.guid != owner_.getPlayerGuid()) {
            owner_.eventBus().publish(events::UnitMoveFlagsChanged{block.guid, block.moveFlags, ~0u});
        }
    }
}

void WorldHandler::onCreatePlayer(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    static const bool kVerboseUpdateObject = core::envFlagEnabled("WOWEE_LOG_UPDATE_OBJECT_VERBOSE", false);

    // For the local player, capture the full initial field state
    if (block.guid == owner_.getPlayerGuid()) {
        owner_.player().fieldsCreated(entity->getFields());
        owner_.inventory().maybeDetectVisibleItemLayout();
    }

    // The name, known from an earlier sighting or asked for, and the visible
    // items. An object that left view and came back is a new one with no
    // name of its own.
    {
        const auto& names = owner_.queryCache().getPlayerNameCache();
        if (auto it = names.find(block.guid); it != names.end()) {
            auto player = std::static_pointer_cast<Player>(entity);
            if (player->getName().empty()) player->setName(it->second);
        }
        owner_.queryCache().queryPlayerName(block.guid);
    }
    if (block.guid != owner_.getPlayerGuid()) {
        owner_.inventory().updateOtherPlayerVisibleItems(block.guid, entity->getFields());
    }

    // Unit fields (PLAYER is a unit)
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    bool unitInitiallyDead = applyUnitFieldsOnCreate(block, unit, ufi);

    // Self-player post-unit-field handling
    if (block.guid == owner_.getPlayerGuid()) {
        // The server has us on a flight the client did not start - entering
        // the world mid-flight. Show it as one: no walking flags, on the mount.
        auto& movement = owner_.movement();
        if ((unit->getUnitFlags() & UNIT_FLAG_TAXI_FLIGHT) != 0 && !movement.isOnTaxiFlight()) {
            owner_.movement().sanitizeMovementForTaxi();
            movement.applyTaxiMountForCurrentNode();
        }
    }
    if (block.guid == owner_.getPlayerGuid()) {
        const auto field = [&](UF f) -> std::optional<uint32_t> {
            auto it = block.fields.find(fieldIndex(f));
            if (it == block.fields.end()) return std::nullopt;
            return it->second;
        };
        owner_.combat().playerCreated(field(UF::UNIT_FIELD_HEALTH), unit->getDynamicFlags(),
                                      field(UF::PLAYER_FLAGS));
    }
    // Pre-WotLK aura sync on initial object create
    if (block.guid == owner_.getPlayerGuid()) {
        owner_.spells().playerAuraFieldsArrived(entity->getFields(), pendingEvents_);
    }

    // Hostility
    unit->setHostile(owner_.isHostileFaction(unit->getFactionTemplate()));

    // Spawn dispatch
    if (unit->getDisplayId() != 0) {
        dispatchEntitySpawn(block.guid, block.objectType, entity, unit, unitInitiallyDead);
        if (block.hasMovement && block.moveFlags != 0 && block.guid != owner_.getPlayerGuid()) {
            owner_.eventBus().publish(events::UnitMoveFlagsChanged{block.guid, block.moveFlags, ~0u});
        }
    }

    // Player stat fields (self only)
    if (block.guid == owner_.getPlayerGuid()) {
        // Auto-detect coinage index using the previous snapshot vs this full snapshot.
        owner_.inventory().detectCoinageField(owner_.player().fields(), block.fields);

        owner_.player().fieldsCreated(block.fields);
        owner_.inventory().detectInventorySlotBases(block.fields);

        if (kVerboseUpdateObject) {
            uint16_t maxField = 0;
            for (const auto& [key, _val] : block.fields) {
                if (key > maxField) maxField = key;
            }
            LOG_INFO("Player update with ", block.fields.size(),
                     " fields (max index=", maxField, ")");
        }

        owner_.player().playerFieldsArrived(block.fields, true, pendingEvents_);
        owner_.inventory().inventoryFieldsArrived(block.fields, true, pendingEvents_);
        owner_.inventory().maybeDetectVisibleItemLayout();
        owner_.spells().extractSkillFields(owner_.player().fields());
        owner_.spells().extractExploredZoneFields(owner_.player().fields());
        owner_.quests().applyQuestStateFromFields(owner_.player().fields());
    }
}

void WorldHandler::onCreateGameObject(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    auto go = std::static_pointer_cast<GameObject>(entity);
    auto itDisp = block.fields.find(fieldIndex(UF::GAMEOBJECT_DISPLAYID));
    if (itDisp != block.fields.end()) {
        go->setDisplayId(itDisp->second);
    }
    auto itEntry = block.fields.find(fieldIndex(UF::OBJECT_FIELD_ENTRY));
    if (itEntry != block.fields.end() && itEntry->second != 0) {
        go->setEntry(itEntry->second);
        if (const auto* info = owner_.queryCache().getCachedGameObjectInfo(itEntry->second)) {
            go->setName(info->name);
        }
        owner_.queryCache().queryGameObjectInfo(itEntry->second, block.guid);
    }

    // Detect transport GameObjects via UPDATEFLAG_TRANSPORT (0x0002)
    LOG_DEBUG("GameObject CREATE: guid=0x", std::hex, block.guid, std::dec,
             " entry=", go->getEntry(), " displayId=", go->getDisplayId(),
             " updateFlags=0x", std::hex, block.updateFlags, std::dec,
             " pos=(", go->getX(), ", ", go->getY(), ", ", go->getZ(), ")");
    if (block.updateFlags & 0x0002) {
        transportGuids_.insert(block.guid);
        LOG_INFO("Detected transport GameObject: 0x", std::hex, block.guid, std::dec,
                 " entry=", go->getEntry(),
                 " displayId=", go->getDisplayId(),
                 " pos=(", go->getX(), ", ", go->getY(), ", ", go->getZ(), ")");
        // TransportSpawned is published once the spawner has made its WMO instance.
    }
    announceGameObject(block.guid, *go);
}

void WorldHandler::announceGameObject(uint64_t guid, const GameObject& go) {
    // What it is doing, which the create block carries as surely as an update
    // does: a door standing open when the player walks into the room says so
    // here and nowhere else. Only the update path read it, so every object came
    // into view in its closed pose and stayed there until something changed it.
    const uint16_t ufBytes1 = fieldIndex(UF::GAMEOBJECT_BYTES_1);
    if (ufBytes1 != 0xFFFF && go.hasField(ufBytes1)) {
        owner_.eventBus().publish(events::GameObjectStateChanged{
            guid, static_cast<uint8_t>(go.getField(ufBytes1) & 0xFF)});
    }
    if (go.getDisplayId() != 0) {
        float goScale = 1.0f;
        {
            uint16_t scaleIdx = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
            if (scaleIdx != 0xFFFF) {
                uint32_t raw = go.getField(scaleIdx);
                if (raw != 0) {
                    goScale = bitsToFloat(raw);
                    if (goScale <= 0.01f || goScale > 100.0f) goScale = 1.0f;
                }
            }
        }
        owner_.eventBus().publish(events::GameObjectSpawned{guid, go.getEntry(), go.getDisplayId(),
            go.getX(), go.getY(), go.getZ(), go.getOrientation(), goScale});
    }
    // Fire transport move callback for transports (position update on re-creation).
    // NOTE: do NOT mark the guid as server-updated here. A CREATE only carries the
    // spawn position, not evidence that the server is authoritatively streaming this
    // transport's motion. Marking it here forces preferServerData=true at spawn
    // resolution, which puts client-animated ships/zeppelins into strict mode and
    // skips the entry->DBC path remap - WotLK ship GO entries don't match
    // TransportAnimation.dbc 1:1, so they were left stationary. Only genuine
    // movement updates (onValuesUpdateGameObject / MOVEMENT blocks) set that flag.
    if (transportGuids_.count(guid)) {
        owner_.eventBus().publish(events::TransportMoved{guid,
            go.getX(), go.getY(), go.getZ(), go.getOrientation()});
    }
}

void WorldHandler::onCreateItem(const UpdateBlock& block, bool& newItemCreated) {
    trackItemOnCreate(block, newItemCreated);
}

namespace {
/// A corpse object's own fields, read the same way at its create and when it
/// is presented again.
struct CorpseFields {
    const Entity& corpse;
    uint32_t operator()(UF f, uint32_t offset = 0) const {
        const uint16_t idx = fieldIndex(f);
        if (idx == 0xFFFF) return 0;
        return corpse.getField(static_cast<uint16_t>(idx + offset));
    }
    uint64_t ownerGuid() const {
        return (static_cast<uint64_t>((*this)(UF::CORPSE_FIELD_OWNER, 1)) << 32) |
               (*this)(UF::CORPSE_FIELD_OWNER);
    }
    bool ownedBy(uint64_t playerGuid) const {
        const uint32_t ownerLow = (*this)(UF::CORPSE_FIELD_OWNER);
        return ownerGuid() == playerGuid ||
               (ownerLow != 0 && ownerLow == static_cast<uint32_t>(playerGuid));
    }
    // Bones are not a body. Application::updateCorpseBones draws them as the
    // race's death skeleton, whoever they belonged to, for as long as the
    // object is in view.
    bool bones() const { return ((*this)(UF::CORPSE_FIELD_FLAGS) & CORPSE_FLAG_BONES) != 0; }
};
} // namespace

void WorldHandler::onCreateCorpse(const UpdateBlock& block, const Entity& corpse) {
    const CorpseFields field{corpse};
    const bool own = field.ownedBy(owner_.getPlayerGuid());

    // Detect player's own corpse object so we have the position even when
    // SMSG_DEATH_RELEASE_LOC hasn't been received (e.g. login as ghost).
    //
    // Not gated on hasMovement, and that gate was a bug of its own: a corpse
    // is a stationary object and its create block need not carry a movement
    // block at all. What reclaiming needs from here is the *guid* -
    // CMSG_RECLAIM_CORPSE names it, and canReclaimCorpse refuses while it is
    // zero - and the position can come from MSG_CORPSE_QUERY, which the
    // login-as-ghost path already sends. Gating the guid on the position
    // meant a ghost who walked back to a corpse that arrived without one
    // could never take it, however close they stood.
    if (own && !field.bones()) {
        owner_.combat().ownCorpseSeen(block.guid, block.hasMovement, block.x, block.y, block.z);
    }
    announceCorpse(block.guid, corpse);
}

void WorldHandler::announceCorpse(uint64_t guid, const Entity& corpse) {
    const CorpseFields field{corpse};
    const bool bones = field.bones();
    if (field.ownedBy(owner_.getPlayerGuid())) {
        if (bones) return;
        // Corpse objects carry ownership and position but not a standalone
        // render model. Reuse the owning character's appearance and equipment
        // under the corpse GUID, then force the queued instance into DEATH.
        auto characterIt = std::find_if(owner_.charactersRef().begin(), owner_.charactersRef().end(),
            [&](const Character& character) { return character.guid == owner_.getPlayerGuid(); });
        if (characterIt != owner_.charactersRef().end()) {
            owner_.eventBus().publish(events::PlayerSpawned{
                guid, 0,
                static_cast<uint8_t>(characterIt->race),
                static_cast<uint8_t>(characterIt->gender),
                characterIt->appearanceBytes,
                characterIt->facialFeatures,
                corpse.getX(), corpse.getY(), corpse.getZ(), corpse.getOrientation()});

            events::PlayerEquipmentResolved worn{.guid = guid};
            const size_t count = std::min<size_t>(19, characterIt->equipment.size());
            for (size_t i = 0; i < count; ++i) {
                worn.displayIds[i] = characterIt->equipment[i].displayModel;
                worn.inventoryTypes[i] = characterIt->equipment[i].inventoryType;
            }
            owner_.eventBus().publish(worn);
        }
        owner_.eventBus().publish(events::UnitDied{guid});
        return;
    }
    if (bones || !corpse.hasPosition()) return;

    // Someone else's body, released but not yet bones. The owner may be far
    // away or offline, so everything it looks like comes off the corpse
    // object itself, the way the original client builds it: race, gender and
    // skin in BYTES_1, face and hair in BYTES_2, the gear as display ids.
    const uint32_t corpseFlags = field(UF::CORPSE_FIELD_FLAGS);
    const uint32_t bytes1 = field(UF::CORPSE_FIELD_BYTES_1);
    const uint32_t bytes2 = field(UF::CORPSE_FIELD_BYTES_2);
    const auto race = static_cast<uint8_t>((bytes1 >> 8) & 0xFF);
    const auto gender = static_cast<uint8_t>((bytes1 >> 16) & 0xFF);
    if (!hasPlayerModel(static_cast<Race>(race))) return;
    // Laid out as PLAYER_BYTES is: skin, face, hair style, hair colour.
    const uint32_t appearance = ((bytes1 >> 24) & 0xFF) | ((bytes2 & 0xFF) << 8) |
                                (((bytes2 >> 8) & 0xFF) << 16) | (((bytes2 >> 16) & 0xFF) << 24);
    const auto facialHair = static_cast<uint8_t>((bytes2 >> 24) & 0xFF);
    owner_.eventBus().publish(events::PlayerSpawned{guid, field(UF::CORPSE_FIELD_DISPLAY_ID), race, gender,
                                                    appearance, facialHair, corpse.getX(), corpse.getY(),
                                                    corpse.getZ(), corpse.getOrientation()});
    {
        events::PlayerEquipmentResolved worn{.guid = guid};
        for (uint32_t i = 0; i < 19; ++i) {
            const uint32_t item = field(UF::CORPSE_FIELD_ITEM, i);
            worn.displayIds[i] = item & 0x00FFFFFFu;
            worn.inventoryTypes[i] = static_cast<uint8_t>(item >> 24);
        }
        constexpr auto kHead = static_cast<size_t>(EquipSlot::HEAD);
        constexpr auto kBack = static_cast<size_t>(EquipSlot::BACK);
        if (corpseFlags & CORPSE_FLAG_HIDE_HELM) { worn.displayIds[kHead] = 0; worn.inventoryTypes[kHead] = 0; }
        if (corpseFlags & CORPSE_FLAG_HIDE_CLOAK) { worn.displayIds[kBack] = 0; worn.inventoryTypes[kBack] = 0; }
        owner_.eventBus().publish(worn);
    }
    owner_.eventBus().publish(events::UnitDied{guid});
}

// ============================================================
// Type-specific VALUES UPDATE handlers
// ============================================================

void WorldHandler::handleDisplayIdChange(const UpdateBlock& block,
                                              const std::shared_ptr<Entity>& entity,
                                              const std::shared_ptr<Unit>& unit,
                                              const UnitFieldUpdateResult& result) {
    if (!result.displayIdChanged || unit->getDisplayId() == 0 ||
        unit->getDisplayId() == result.oldDisplayId)
        return;

    bool isDeadNow = isUnitCorpseState(
        unit->getHealth(), unit->getMaxHealth(), unit->getDynamicFlags());
    dispatchEntitySpawn(block.guid, entity->getType(), entity, unit,
                        isDeadNow && !result.npcDeathNotified);
    if (owner_.hasAddonEventListener()) {
        std::string uid;
        if (block.guid == owner_.combat().getTargetGuid()) uid = "target";
        else if (block.guid == owner_.combat().getFocusGuid()) uid = "focus";
        else if (block.guid == owner_.petGuid()) uid = "pet";
        if (!uid.empty())
            pendingEvents_.emit("UNIT_MODEL_CHANGED", {uid});
    }
}

void WorldHandler::onValuesUpdateUnit(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    UnitFieldUpdateResult result = applyUnitFieldsOnUpdate(block, entity, unit, ufi);
    handleDisplayIdChange(block, entity, unit, result);

    // A corpse that has been looted empty has nothing left to offer, so drop it.
    // Skinnable corpses stay until they have been skinned. Done here rather than
    // inside the field loop so the entity is not removed while it is being read.
    constexpr uint32_t UNIT_FLAG_SKINNABLE = 0x04000000;
    if (result.lootableCleared && unit->getHealth() == 0 &&
        (unit->getUnitFlags() & UNIT_FLAG_SKINNABLE) == 0) {
        owner_.despawnCreatureLocally(block.guid);
    }
}

void WorldHandler::onValuesUpdatePlayer(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    // Other player visible items
    if (block.guid != owner_.getPlayerGuid()) {
        owner_.inventory().updateOtherPlayerVisibleItems(block.guid, entity->getFields());
    }

    // Unit field update (player IS a unit)
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    UnitFieldUpdateResult result = applyUnitFieldsOnUpdate(block, entity, unit, ufi);

    // Pre-WotLK aura sync from UNIT_FIELD_AURAS when those fields are updated
    if (block.guid == owner_.getPlayerGuid()) {
        owner_.spells().playerAuraFieldsArrived(entity->getFields(), pendingEvents_);
    }

    // Display ID changed - re-spawn/model-change
    handleDisplayIdChange(block, entity, unit, result);

    // Self-player stat/inventory/quest field updates
    if (block.guid == owner_.getPlayerGuid()) {
        const bool needCoinageDetectSnapshot = owner_.inventory().expectedMoneyDelta() != 0;
        FlatFieldMap oldFieldsSnapshot;
        if (needCoinageDetectSnapshot) {
            oldFieldsSnapshot = owner_.player().fields();
        }
        if (block.hasMovement && block.runSpeed > 0.1f && block.runSpeed < 100.0f) {
            // Speed is independent of mount ownership: slows and ordinary movement
            // snapshots may report base-speed values while UNIT_FIELD_MOUNTDISPLAYID
            // still authoritatively says the player is mounted.
            owner_.movement().applyServerMovementSpeeds(
                block.walkSpeed, block.runSpeed, block.runBackSpeed,
                block.swimSpeed, block.swimBackSpeed, block.flightSpeed,
                block.flightBackSpeed, block.turnRate, block.pitchRate);
        }
        owner_.player().fieldsUpdated(block.fields);
        if (needCoinageDetectSnapshot) {
            owner_.inventory().detectCoinageField(oldFieldsSnapshot, owner_.player().fields());
        }
        owner_.inventory().maybeDetectVisibleItemLayout();
        owner_.inventory().detectInventorySlotBases(block.fields);

        owner_.player().playerFieldsArrived(block.fields, false, pendingEvents_);
        owner_.inventory().inventoryFieldsArrived(block.fields, false, pendingEvents_);
        owner_.spells().extractSkillFields(owner_.player().fields());
        owner_.spells().extractExploredZoneFields(owner_.player().fields());
        owner_.quests().applyQuestStateFromFields(owner_.player().fields());
    }
}

void WorldHandler::onValuesUpdateItem(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    updateItemOnValuesUpdate(block, entity);
}

void WorldHandler::onValuesUpdateGameObject(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    if (block.hasMovement) {
        if (transportGuids_.count(block.guid)) {
            serverUpdatedTransportGuids_.insert(block.guid);
            owner_.eventBus().publish(events::TransportMoved{block.guid, entity->getX(), entity->getY(),
                                                             entity->getZ(), entity->getOrientation()});
        } else {
            owner_.eventBus().publish(events::GameObjectMoved{block.guid, entity->getX(), entity->getY(),
                                                              entity->getZ(), entity->getOrientation()});
        }
    }

    // Detect GO state changes from GAMEOBJECT_BYTES_1 (packed: byte0=state, byte1=type, byte2=artKit, byte3=animProgress)
    const uint16_t ufGoBytes1 = fieldIndex(UF::GAMEOBJECT_BYTES_1);
    if (ufGoBytes1 != 0xFFFF) {
        auto itB = block.fields.find(ufGoBytes1);
        if (itB != block.fields.end()) {
            uint8_t goState = static_cast<uint8_t>(itB->second & 0xFF);
            owner_.eventBus().publish(events::GameObjectStateChanged{block.guid, goState});
        }
    }

    applyTransportRouteClock(block);
}

void WorldHandler::applyTransportRouteClock(const UpdateBlock& block) {
    // A moving transport publishes where it is on its route: LEVEL is the route's
    // period in milliseconds, and the high int16 of DYNAMIC is how far through
    // that period it currently is, as a fraction of 65535.
    //
    // Without this the client animated on a clock it invented from distance over
    // speed, which is why a ferry could lap its shore several times while the
    // server's schedule caught up, and why a rider's world position - which the
    // client composes from its own idea of where the hull is - could disagree
    // with the server's.
    //
    // Both fields are WotLK-only. Nothing earlier published a transport's phase,
    // so on those expansions fieldIndex returns 0xFFFF and this does nothing.
    auto* tm = owner_.getTransportManager();
    if (!tm || !tm->getTransport(block.guid)) return;

    const uint16_t ufLevel = fieldIndex(UF::GAMEOBJECT_LEVEL);
    const uint16_t ufDynamic = fieldIndex(UF::GAMEOBJECT_DYNAMIC);
    if (ufLevel == 0xFFFF || ufDynamic == 0xFFFF) return;

    auto itPeriod = block.fields.find(ufLevel);
    auto itDynamic = block.fields.find(ufDynamic);
    if (itPeriod == block.fields.end() || itDynamic == block.fields.end()) return;

    const uint32_t periodMs = itPeriod->second;
    // Signed on the wire: -1 means "this object has no path progress to report",
    // which every non-transport GameObject sends.
    const int16_t rawProgress = static_cast<int16_t>((itDynamic->second >> 16) & 0xFFFF);
    if (periodMs == 0 || rawProgress < 0) return;

    const float phase = static_cast<float>(rawProgress) / 65535.0f;
    tm->applyServerRouteClock(block.guid, phase, periodMs);
}

// ============================================================
// Update type handlers
// ============================================================

void WorldHandler::handleCreateObject(const UpdateBlock& block, bool& newItemCreated) {
    if (block.guid == owner_.getPlayerGuid()) owner_.spells().companionFieldsArrived(block.fields);
    pendingEvents_.clear();

    // 3a: Create entity from block type
    std::shared_ptr<Entity> entity = createEntityFromBlock(block);

    // Set position from movement block (server → canonical)
    if (block.hasMovement) {
        glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
        float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
        entity->setPosition(pos.x, pos.y, pos.z, oCanonical);
        LOG_DEBUG("  Position: (", pos.x, ", ", pos.y, ", ", pos.z, ")");
        applyBlockMovementSpeeds(block, *entity);
        if (block.guid == owner_.getPlayerGuid() && block.runSpeed > 0.1f && block.runSpeed < 100.0f) {
            owner_.movement().applyServerMovementSpeeds(
                block.walkSpeed, block.runSpeed, block.runBackSpeed,
                block.swimSpeed, block.swimBackSpeed, block.flightSpeed,
                block.flightBackSpeed, block.turnRate, block.pitchRate);
        }
        // 3b: Track player-on-transport state
        if (block.guid == owner_.getPlayerGuid()) {
            if (auto aboard = owner_.movement().serverPlacedPlayer(block, pos, oCanonical, false))
                entity->setPosition(aboard->x, aboard->y, aboard->z, oCanonical);
        }
        // 3i: Track transport-relative children so they follow parent transport motion.
        updateNonPlayerTransportAttachment(block, entity, block.objectType);
    }

    // Set fields
    for (const auto& field : block.fields) {
        entity->setField(field.first, field.second);
    }

    // Add to manager
    entityManager.addEntity(block.guid, entity);

    // Dispatch to type-specific handler
    auto* handler = getTypeHandler(block.objectType);
    if (handler) handler->onCreate(block, entity, newItemCreated);

    flushPendingEvents();
}

void WorldHandler::handleValuesUpdate(const UpdateBlock& block) {
    if (block.guid == owner_.getPlayerGuid()) owner_.spells().companionFieldsArrived(block.fields);
    auto entity = entityManager.getEntity(block.guid);
    if (!entity) {
        // Item/container entities may be absent from entityManager (e.g. server
        // only sent a partial update) but the inventory still holds them.
        // Process field updates so durability/stack changes from repair aren't lost.
        if (owner_.inventory().holdsItemObject(block.guid)) {
            pendingEvents_.clear();
            updateItemOnValuesUpdate(block, entity);
            flushPendingEvents();
            LOG_DEBUG("Updated orphan item fields: 0x", std::hex, block.guid, std::dec);
        }
        return;
    }
    pendingEvents_.clear();

    // Position update (common)
    if (block.hasMovement) {
        glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
        float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
        entity->setPosition(pos.x, pos.y, pos.z, oCanonical);
        updateNonPlayerTransportAttachment(block, entity, entity->getType());
    }

    // Set fields (common)
    for (const auto& field : block.fields) {
        entity->setField(field.first, field.second);
    }

    // Dispatch to type-specific handler
    auto* handler = getTypeHandler(entity->getType());
    if (handler) handler->onValuesUpdate(block, entity);

    flushPendingEvents();
    LOG_DEBUG("Updated entity fields: 0x", std::hex, block.guid, std::dec);
}

void WorldHandler::handleMovementUpdate(const UpdateBlock& block) {
            // Diagnostic: Log if we receive MOVEMENT blocks for transports
            if (transportGuids_.count(block.guid)) {
                LOG_INFO("MOVEMENT update for transport 0x", std::hex, block.guid, std::dec,
                         " pos=(", block.x, ", ", block.y, ", ", block.z, ")");
            }

            // Update entity position (server → canonical)
            auto entity = entityManager.getEntity(block.guid);
            if (entity) {
                glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
                float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
                entity->setPosition(pos.x, pos.y, pos.z, oCanonical);
                LOG_DEBUG("Updated entity position: 0x", std::hex, block.guid, std::dec);
                applyBlockMovementSpeeds(block, *entity);

                updateNonPlayerTransportAttachment(block, entity, entity->getType());

                // 3b: Track player-on-transport state from MOVEMENT updates
                if (block.guid == owner_.getPlayerGuid()) {
                    if (auto aboard = owner_.movement().serverPlacedPlayer(block, pos, oCanonical, true))
                        entity->setPosition(aboard->x, aboard->y, aboard->z, oCanonical);
                }

                // Fire transport move callback if this is a known transport
                if (transportGuids_.count(block.guid)) {
                    serverUpdatedTransportGuids_.insert(block.guid);
                    owner_.eventBus().publish(events::TransportMoved{block.guid, pos.x, pos.y, pos.z, oCanonical});
                }
                // Fire move callback for non-transport gameobjects.
                if (entity->getType() == ObjectType::GAMEOBJECT &&
                    transportGuids_.count(block.guid) == 0) {
                    owner_.eventBus().publish(events::GameObjectMoved{block.guid, entity->getX(), entity->getY(),
                                                                      entity->getZ(), entity->getOrientation()});
                }
                // Fire move callback for non-player units (creatures).
                // SMSG_MONSTER_MOVE handles smooth interpolated movement, but many
                // servers (especially vanilla) communicate NPC positions
                // via MOVEMENT blocks instead. Use duration=0 for an instant snap.
                if (block.guid != owner_.getPlayerGuid() &&
                    entity->getType() == ObjectType::UNIT &&
                    transportGuids_.count(block.guid) == 0) {
                    owner_.eventBus().publish(events::CreatureMoved{block.guid, pos.x, pos.y, pos.z, 0});
                }
            } else {
                LOG_WARNING("MOVEMENT update for unknown entity: 0x", std::hex, block.guid, std::dec);
            }
}

void WorldHandler::finalizeUpdateObjectBatch(bool newItemCreated) {
    owner_.combat().invalidateTabCycle();
    // Entity count logging disabled

    owner_.inventory().updateBatchFinished(newItemCreated, owner_.player().fields());
}

void WorldHandler::handleCompressedUpdateObject(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_COMPRESSED_UPDATE_OBJECT, packet size: ", packet.getSize());

    // First 4 bytes = decompressed size
    if (packet.getSize() < 4) {
        LOG_WARNING("SMSG_COMPRESSED_UPDATE_OBJECT too small");
        return;
    }

    uint32_t decompressedSize = packet.readUInt32();
    LOG_DEBUG("  Decompressed size: ", decompressedSize);

    // Capital cities and large raids can produce very large update packets.
    // The real WoW client handles up to ~10MB; 5MB covers all practical cases.
    if (decompressedSize == 0 || decompressedSize > 5 * 1024 * 1024) {
        LOG_WARNING("Invalid decompressed size: ", decompressedSize);
        return;
    }

    // Remaining data is zlib compressed
    size_t compressedSize = packet.getRemainingSize();
    const uint8_t* compressedData = packet.getData().data() + packet.getReadPos();

    // Decompress
    std::vector<uint8_t> decompressed(decompressedSize);
    uLongf destLen = decompressedSize;
    int ret = uncompress(decompressed.data(), &destLen, compressedData, compressedSize);

    if (ret != Z_OK) {
        LOG_WARNING("Failed to decompress UPDATE_OBJECT: zlib error ", ret);
        return;
    }

    // Create packet from decompressed data and parse it
    network::Packet decompressedPacket(wireOpcode(Opcode::SMSG_UPDATE_OBJECT), decompressed);
    handleUpdateObject(decompressedPacket);
}
void WorldHandler::handleDestroyObject(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_DESTROY_OBJECT");

    DestroyObjectData data;
    if (!DestroyObjectParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_DESTROY_OBJECT");
        return;
    }

    // Remove entity
    if (entityManager.hasEntity(data.guid)) {
        if (transportGuids_.count(data.guid) > 0) {
            const bool playerAboardNow = (owner_.movement().getPlayerTransportGuid() == data.guid);
            const bool stickyAboard = owner_.movement().recentlyAboard(data.guid);
            const bool movementSaysAboard = (owner_.movementInfoRef().transportGuid == data.guid);
            // CMaNGOS judges visibility using the transport's static DB spawn
            // coordinate, which can be far from where a client-animated transport
            // currently is - keep all transports alive unconditionally on pre-WotLK.
            // AzerothCore (WotLK) sends legitimate destroy/recreate cycles, so only
            // preserve if the player is actually aboard.
            if (isPreWotlk() || playerAboardNow || stickyAboard || movementSaysAboard) {
                serverUpdatedTransportGuids_.erase(data.guid);
                LOG_INFO("Preserving transport on destroy: 0x", std::hex, data.guid, std::dec,
                         " now=", playerAboardNow,
                         " sticky=", stickyAboard,
                         " movement=", movementSaysAboard);
                return;
            }
        }
        // Mirror out-of-range handling: invoke render-layer despawn callbacks before entity removal.
        auto entity = entityManager.getEntity(data.guid);
        if (entity) {
            if (entity->getType() == ObjectType::UNIT) {
                owner_.eventBus().publish(events::CreatureDespawned{data.guid});
            } else if (entity->getType() == ObjectType::PLAYER) {
                // Player entities also need renderer cleanup on DESTROY_OBJECT, not just out-of-range.
                owner_.eventBus().publish(events::PlayerDespawned{data.guid});
                owner_.inventory().forgetPlayerEquipment(data.guid);
                owner_.queryCache().playerLeftView(data.guid);
            } else if (entity->getType() == ObjectType::GAMEOBJECT) {
                owner_.eventBus().publish(events::GameObjectDespawned{data.guid});
            } else if (entity->getType() == ObjectType::CORPSE) {
                owner_.eventBus().publish(events::PlayerDespawned{data.guid});
            }
        }
        if (transportGuids_.count(data.guid) > 0) {
            transportGuids_.erase(data.guid);
            serverUpdatedTransportGuids_.erase(data.guid);
            if (owner_.movement().getPlayerTransportGuid() == data.guid) {
                owner_.movement().clearPlayerTransport();
            }
        }
        owner_.movement().clearTransportAttachment(data.guid);
        entityManager.removeEntity(data.guid);
        LOG_INFO("Destroyed entity: 0x", std::hex, data.guid, std::dec,
                 " (", (data.isDeath ? "death" : "despawn"), ")");
    } else {
        LOG_DEBUG("Destroy object for unknown entity: 0x", std::hex, data.guid, std::dec);
    }

    // Clean up auto-attack and target if destroyed entity was our target
    if (data.guid == owner_.combat().getAutoAttackTargetGuid()) {
        owner_.stopAutoAttack();
    }
    if (data.guid == owner_.combat().getTargetGuid()) {
        owner_.combat().targetGone();
    }
    owner_.combat().removeHostileAttacker(data.guid);

    // Remove online item/container tracking
    if (owner_.inventory().forgetItemObject(data.guid)) {
        owner_.inventory().rebuildOnlineInventory();
    }

    // Clean up quest giver status
    // QuestHandler's map, which is the one every reader forwards to.
    // GameHandler's same-named member is dead, so erasing there left a
    // despawned quest giver's mark behind on whatever took its place.
    owner_.quests().forgetQuestGiver(data.guid);

    // Remove combat text entries referencing the destroyed entity so floating
    // damage numbers don't linger after the source/target despawns.
    owner_.combat().removeCombatTextForGuid(data.guid);

    // Clean up unit cast owner_.getState() (cast bar) for the destroyed unit
    owner_.spells().removeUnitCastState(data.guid);
    // Clean up cached auras
    owner_.spells().removeUnitAuraCache(data.guid);

    owner_.combat().invalidateTabCycle();
}

// Name Queries
// ============================================================

void WorldHandler::playerNamed(uint64_t guid, const std::string& name) {
    auto entity = entityManager.getEntity(guid);
    if (entity && entity->getType() == ObjectType::PLAYER) {
        std::static_pointer_cast<Player>(entity)->setName(name);
    }
    // UNIT_NAME_UPDATE so nameplate/unit frame addons know the name is available
    if (owner_.hasAddonEventListener()) {
        std::string unitId;
        if (guid == owner_.combat().getTargetGuid()) unitId = "target";
        else if (guid == owner_.combat().getFocusGuid()) unitId = "focus";
        else if (guid == owner_.getPlayerGuid()) unitId = "player";
        if (!unitId.empty()) owner_.fireAddonEvent("UNIT_NAME_UPDATE", {unitId});
    }
}

void WorldHandler::creatureNamed(const CreatureQueryResponseData& data) {
    for (auto& [guid, entity] : entityManager.getEntities()) {
        if (entity->getType() != ObjectType::UNIT) continue;
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (unit->getEntry() != data.entry) continue;
        unit->setName(data.name);
        // The rank came with this response, and UnitClassification reads it
        // from the cache just filled. A unit targeted before its query came
        // back answered "normal" until now - so an elite or a rare drew a plain
        // border and kept it, because the frame only rechecks when told to.
        //
        // Only for a unit the interface has a token for: the frame compares the
        // argument against its own unit and ignores anything else, and most of
        // a zone is neither targeted nor focused.
        const std::string unitId = owner_.guidToUnitId(guid);
        if (!unitId.empty() && owner_.hasAddonEventListener()) {
            owner_.fireAddonEvent("UNIT_CLASSIFICATION_CHANGED", {unitId});
        }
    }
}

void WorldHandler::gameObjectNamed(const GameObjectQueryResponseData& data) {
    for (auto& [guid, entity] : entityManager.getEntities()) {
        if (entity->getType() != ObjectType::GAMEOBJECT) continue;
        auto go = std::static_pointer_cast<GameObject>(entity);
        if (go->getEntry() == data.entry) go->setName(data.name);
    }
}

} // namespace game
} // namespace wowee
