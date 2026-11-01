#pragma once

#include "game/frame_xml_events.hpp"

#include "game/world_packet_data.hpp"
#include "game/entity.hpp"
#include "game/player_appearance.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <glm/vec3.hpp>

namespace wowee::core { class EventBus; }
namespace wowee::pet { class IPetFeed; }

namespace wowee {
namespace game {

class EmoteTable;
class PacketParsers;
class PacketSender;
class TransportManager;
struct Character;
enum class WorldState;

/// What the objects in view ask of the rest of the game, and whom they tell
/// of the parts of a field update another area owns. GameHandler implements
/// it and hands each call to the owner, so the interface hears of one update
/// in the order of its fields.
class WorldQueries {
public:
    virtual ~WorldQueries() = default;

    // ---- The session ----
    [[nodiscard]] virtual WorldState worldState() const = 0;
    [[nodiscard]] virtual uint64_t characterGuid() const = 0;
    /// The character list's entry for the character in the world, or null.
    virtual Character* listedCharacter() = 0;
    [[nodiscard]] virtual PacketParsers& packetParsers() = 0;
    /// "target", "focus", "party1"... for a guid the interface has a token for.
    [[nodiscard]] virtual std::string unitIdFor(uint64_t guid) const = 0;
    [[nodiscard]] virtual bool hostileFaction(uint32_t factionTemplateId) const = 0;
    /// The transports' own motion. Never null in a running game.
    [[nodiscard]] virtual TransportManager* transports() = 0;

    // ---- The player's movement and mount ----
    /// The transport the player's movement says they are on, 0 for none.
    [[nodiscard]] virtual uint64_t movementTransportGuid() const = 0;
    [[nodiscard]] virtual uint64_t playerTransportGuid() const = 0;
    /// The player left `guid` within the last moments: a server update that
    /// still has them aboard is late, not news.
    [[nodiscard]] virtual bool recentlyAboard(uint64_t guid) const = 0;
    virtual void clearPlayerTransport() = 0;
    virtual void setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                        const glm::vec3& localOffset, bool hasLocalOrientation,
                                        float localOrientation, bool offsetNeedsTransportResolution) = 0;
    virtual void clearTransportAttachment(uint64_t childGuid) = 0;
    /// Where the server put the player, when the client must take it.
    virtual std::optional<glm::vec3> serverPlacedPlayer(const UpdateBlock& block, const glm::vec3& canonicalPos,
                                                        float canonicalOrientation, bool movementUpdate) = 0;
    virtual void applyServerMovementSpeeds(float walk, float run, float runBack, float swim,
                                           float swimBack, float flight, float flightBack,
                                           float turn, float pitch) = 0;
    /// The player's create block has them on a taxi flight the client did
    /// not start.
    virtual void playerCreatedInFlight() = 0;
    virtual void playerMountFieldArrived(uint32_t displayId, const FlatFieldMap& blockFields,
                                         FrameXmlEvents& packetEvents) = 0;

    // ---- The player's sheet and body ----
    [[nodiscard]] virtual const FlatFieldMap& playerFields() const = 0;
    virtual void playerFieldsCreated(const FlatFieldMap& fields) = 0;
    virtual void playerFieldsUpdated(const FlatFieldMap& fields) = 0;
    virtual void playerFieldsArrived(const FlatFieldMap& fields, bool isCreate, FrameXmlEvents& packetEvents) = 0;
    virtual void shapeshiftFormArrived(uint8_t form, FrameXmlEvents& packetEvents) = 0;
    /// UNIT_FIELD_FLAGS on the player, in an update: the fight and the stun.
    virtual void playerUnitFlagsArrived(uint32_t previous, uint32_t flags, FrameXmlEvents& packetEvents) = 0;
    virtual void playerVisibilityFlagsArrived(uint8_t previous, uint8_t flags) = 0;
    virtual void playerDynamicFlagsArrived(uint32_t previous, uint32_t flags, FrameXmlEvents& packetEvents) = 0;
    virtual void playerObjectCreated(std::optional<uint32_t> health, uint32_t dynamicFlags,
                                     std::optional<uint32_t> playerFlags) = 0;
    virtual void unitHealthArrived(uint64_t guid, uint32_t previous, uint32_t health,
                                   FrameXmlEvents& packetEvents) = 0;
    virtual void ownCorpseSeen(uint64_t guid, bool hasPosition, float serverX, float serverY,
                               float serverZ) = 0;
    virtual void playerAuraFieldsArrived(const FlatFieldMap& fields, FrameXmlEvents& packetEvents) = 0;
    virtual void skillFieldsArrived(const FlatFieldMap& fields) = 0;
    virtual void exploredZoneFieldsArrived(const FlatFieldMap& fields) = 0;
    virtual void companionFieldsArrived(const FlatFieldMap& blockFields) = 0;
    virtual void questFieldsArrived(const FlatFieldMap& fields) = 0;

    // ---- The fight ----
    [[nodiscard]] virtual uint64_t currentTargetGuid() const = 0;
    [[nodiscard]] virtual uint64_t focusTargetGuid() const = 0;
    [[nodiscard]] virtual uint64_t autoAttackTargetGuid() const = 0;
    virtual void stopAutoAttack() = 0;
    /// The target left view.
    virtual void targetGone() = 0;
    /// The units in view changed: Tab's cycle is built again.
    virtual void invalidateTabCycle() = 0;
    virtual void removeHostileAttacker(uint64_t guid) = 0;
    virtual void removeCombatTextForGuid(uint64_t guid) = 0;

    // ---- What other parts keep of an object ----
    virtual void forgetUnitAuras(uint64_t guid) = 0;
    virtual void forgetUnitCast(uint64_t guid) = 0;
    virtual void forgetQuestGiver(uint64_t guid) = 0;
    [[nodiscard]] virtual uint64_t petGuid() const = 0;
    /// Where the pet's fields go; null when the client was built without it.
    virtual pet::IPetFeed* petFeed() = 0;
    /// Take a creature out of view without the server saying so.
    virtual void despawnCreatureLocally(uint64_t guid) = 0;

    // ---- The query caches ----
    [[nodiscard]] virtual const std::unordered_map<uint64_t, std::string>& playerNameCache() const = 0;
    [[nodiscard]] virtual std::string creatureName(uint32_t entry) const = 0;
    [[nodiscard]] virtual const GameObjectQueryResponseData* gameObjectInfo(uint32_t entry) const = 0;
    virtual void requestPlayerName(uint64_t guid) = 0;
    virtual void requestCreatureInfo(uint32_t entry, uint64_t guid) = 0;
    virtual void requestGameObjectInfo(uint32_t entry, uint64_t guid) = 0;
    /// A player left view: a name asked for and not answered is asked again.
    virtual void playerLeftView(uint64_t guid) = 0;

    // ---- Items and what other players wear ----
    [[nodiscard]] virtual bool holdsItemObject(uint64_t guid) const = 0;
    virtual bool trackItemObject(uint64_t guid, bool isContainer, const FlatFieldMap& fields) = 0;
    virtual void itemObjectChanged(uint64_t guid, bool isContainer, const FlatFieldMap& fields,
                                   FrameXmlEvents& packetEvents) = 0;
    virtual bool forgetItemObject(uint64_t guid) = 0;
    virtual void inventoryFieldsArrived(const FlatFieldMap& fields, bool isCreate, FrameXmlEvents& packetEvents) = 0;
    virtual void detectInventorySlotBases(const FlatFieldMap& fields) = 0;
    virtual void detectCoinageField(const FlatFieldMap& before, const FlatFieldMap& after) = 0;
    [[nodiscard]] virtual uint32_t expectedMoneyDelta() const = 0;
    virtual void maybeDetectVisibleItemLayout() = 0;
    virtual void rebuildOnlineInventory() = 0;
    virtual void updateBatchFinished(bool newItemCreated, const FlatFieldMap& playerFields) = 0;
    virtual void updateOtherPlayerVisibleItems(uint64_t guid, const FlatFieldMap& fields) = 0;
    virtual void forgetPlayerEquipment(uint64_t guid) = 0;
    virtual void emitAllOtherPlayerEquipment() = 0;
};

/// The objects in view, as SMSG_UPDATE_OBJECT, its compressed form and
/// SMSG_DESTROY_OBJECT describe them: their lifetime, their fields, and how
/// each is presented when it comes into view. The parts of a field update
/// another area owns - the player's sheet, combat, the mount, the inventory -
/// are handed to that owner as the fields are read, in their order.
class WorldHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    WorldHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                 WorldQueries& queries, const EmoteTable& emotes);

    void registerOpcodes(DispatchTable& table);

    // --- Entity Manager access ---
    EntityManager& getEntityManager() { return entityManager; }
    [[nodiscard]] const EntityManager& getEntityManager() const { return entityManager; }

    /// Present every object in view again, as its create block did: the
    /// model, worn equipment, a rider's mount, the death pose, a door's state
    /// and a released corpse. The client asks for this when it builds a new
    /// scene - entering a map, or a far teleport within one - because the
    /// server does not send an object that is still in range a second time,
    /// and a create that arrived before the scene existed had nowhere to go.
    /// Sends nothing to the server.
    void announceObjectsInView();
    /// Offline scene input, through the ordinary create/values/despawn path.
    /// The caller owns the sampled clock and supplies positions separately.
    void applyRecordedObjects(const UpdateObjectData& data);

    // --- What the query caches learned, for the objects in view ---
    /// A player's name arrived: the player in view takes it, and the
    /// interface is told for a unit it has a token for.
    void playerNamed(uint64_t guid, const std::string& name);
    /// A creature template arrived: every unit of it in view takes its name,
    /// and the frames showing one redraw its classification.
    void creatureNamed(const CreatureQueryResponseData& data);
    /// A game object template arrived: every object of it in view takes its
    /// name.
    void gameObjectNamed(const GameObjectQueryResponseData& data);

    // --- Transport GUID tracking ---
    [[nodiscard]] bool isTransportGuid(uint64_t guid) const { return transportGuids_.count(guid) > 0; }
    [[nodiscard]] bool hasServerTransportUpdate(uint64_t guid) const { return serverUpdatedTransportGuids_.count(guid) > 0; }

    // --- Update object work queue ---
    void enqueueUpdateObjectWork(UpdateObjectData&& data);
    void processPendingUpdateObjectWork(const std::chrono::steady_clock::time_point& start,
                                        float budgetMs);
    [[nodiscard]] bool hasPendingUpdateObjectWork() const { return !pendingUpdateObjectWork_.empty(); }

    // --- Reset all state (called on disconnect / character switch) ---
    void clearAll();

private:
    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    WorldQueries& queries_;
    const EmoteTable& emotes_;

    // --- Entity tracking ---
    EntityManager entityManager;              // Manages all entities in view


    // --- Update Object work queue ---
    struct PendingUpdateObjectWork {
        UpdateObjectData data;
        size_t nextBlockIndex = 0;
        size_t nextOutOfRangeIndex = 0;  // out-of-range removals done so far
        float outOfRangeMs = 0.0f;       // spent on them, across frames
        bool outOfRangeProcessed = false;
        bool newItemCreated = false;
    };
    std::deque<PendingUpdateObjectWork> pendingUpdateObjectWork_;

    // --- Transport GUID tracking ---
    std::unordered_set<uint64_t> transportGuids_;  // GUIDs of known transport GameObjects
    std::unordered_set<uint64_t> serverUpdatedTransportGuids_;

    // --- Packet handlers ---
    void handleUpdateObject(network::Packet& packet);
    void handleCompressedUpdateObject(network::Packet& packet);
    void handleDestroyObject(network::Packet& packet);


private:

    // --- Entity lifecycle ---
    /// Removes out-of-range objects from `next` on until `deadline`, at least
    /// one per call; returns the index to resume from. Times what each kind
    /// costs into `costs` (creature, player, game object, other).
    size_t processOutOfRangeObjectsUntil(const std::vector<uint64_t>& guids, size_t next,
                                         std::chrono::steady_clock::time_point deadline,
                                         float costs[4]);
    void applyUpdateObjectBlock(const UpdateBlock& block, bool& newItemCreated);
    void finalizeUpdateObjectBatch(bool newItemCreated);

public:
    /// What a player in view looks like, read from the fields their object
    /// holds. The spawn path reads it once for the model; a portrait or an
    /// inspect window asks here later. Null for anything not a player.
    [[nodiscard]] std::optional<PlayerAppearance> appearanceOf(uint64_t guid) const;

private:

    void handleCreateObject(const UpdateBlock& block, bool& newItemCreated);
    void handleValuesUpdate(const UpdateBlock& block);
    void handleMovementUpdate(const UpdateBlock& block);
    /// Seed a transport's route phase from the server's own path clock, so the
    /// client stops animating on a period it invented for itself.
    void applyTransportRouteClock(const UpdateBlock& block);

    // Update transport-relative child attachment (non-player entities).
    //     Consolidates identical logic from CREATE/VALUES/MOVEMENT handlers.
    void updateNonPlayerTransportAttachment(const UpdateBlock& block,
                                            const std::shared_ptr<Entity>& entity,
                                            ObjectType entityType);
    // Rebuild playerAuras_ from UNIT_FIELD_AURAS (Classic/TBC-era clients).
    //     Consolidates identical logic from CREATE and VALUES handlers.
    // Detect mount/dismount from UNIT_FIELD_MOUNTDISPLAYID changes (self-player only).
    //     Consolidates identical logic from CREATE and VALUES handlers.
    void detectPlayerMountChange(uint32_t newMountDisplayId,
                                 const FlatFieldMap& blockFields);

    // Cached field indices resolved once per handler call to avoid repeated lookups.
    struct UnitFieldIndices {
        uint16_t health, maxHealth, powerBase, maxPowerBase;
        uint16_t level, faction, flags, dynFlags, auraState;
        uint16_t displayId, nativeDisplayId, mountDisplayId, npcFlags, npcEmoteState;
        /// How wide a unit is and how far it reaches, for edge-to-edge range.
        uint16_t boundingRadius, combatReach;
        uint16_t bytes0, bytes1;
        /// A hunter pet carries its own experience, on the pet's unit rather
        /// than on the player's.
        uint16_t petXp, petNextLevelXp;
        /// ...and its own stats and resistances, which the pet paperdoll reads
        /// through UnitStat("pet") and UnitResistance("pet"). Armor is
        /// resistances index 0.
        uint16_t stat0, resistances;
        /// ...and what it hits for, which the pet tab's damage and
        /// attack-power lines read.
        uint16_t attackPower, minDamage, maxDamage;
        static UnitFieldIndices resolve();
    };
    struct UnitFieldUpdateResult {
        bool healthChanged = false;
        /// Separately, because a frame registers for the two apart: the value
        /// and the scale it is drawn against are different questions.
        bool maxHealthChanged = false;
        bool powerChanged = false;
        /// Which power moved, as an index into the power tables, or -1. The
        /// event a WotLK interface listens for is named after the power, so
        /// knowing that something changed is not enough to tell it.
        int  powerTypeChanged = -1;
        int  maxPowerTypeChanged = -1;
        bool displayIdChanged = false;
        bool npcDeathNotified = false;
        bool npcRespawnNotified = false;
        // Set when UNIT_DYNFLAG_LOOTABLE went away this update - the corpse has
        // been looted empty. Acted on after the field loop, never inside it.
        bool lootableCleared = false;
        uint32_t oldDisplayId = 0;
    };

    // Entity factory - creates the correct Entity subclass for the given block.
    std::shared_ptr<Entity> createEntityFromBlock(const UpdateBlock& block);
    // Track player-on-transport state from movement blocks.
    /// Take the server's position for the player when the two have diverged
    /// beyond anything lag can explain. Does nothing in the ordinary case,
    /// where the client owns its position and the server echoes it back.

    // Apply unit fields during CREATE - returns true if entity is initially dead.
    bool applyUnitFieldsOnCreate(const UpdateBlock& block,
                                  std::shared_ptr<Unit>& unit,
                                  const UnitFieldIndices& ufi);
    // Apply unit fields during VALUES - returns change tracking result.
    UnitFieldUpdateResult applyUnitFieldsOnUpdate(const UpdateBlock& block,
                                                    const std::shared_ptr<Entity>& entity,
                                                    std::shared_ptr<Unit>& unit,
                                                    const UnitFieldIndices& ufi);
    // Dispatch spawn callbacks (creature/player) - deduplicates CREATE and VALUES paths.
    void dispatchEntitySpawn(uint64_t guid, ObjectType objectType,
                              const std::shared_ptr<Entity>& entity,
                              const std::shared_ptr<Unit>& unit, bool isDead);
    // The presentation half of each create, shared by the create handlers and
    // announceObjectsInView.
    void announceUnit(uint64_t guid, ObjectType objectType,
                      const std::shared_ptr<Entity>& entity,
                      const std::shared_ptr<Unit>& unit, bool isDead);
    void announceGameObject(uint64_t guid, const GameObject& go);
    void announceCorpse(uint64_t guid, const Entity& corpse);
    // Track item/container on CREATE.
    void trackItemOnCreate(const UpdateBlock& block, bool& newItemCreated);
    // Update item fields on VALUES update.
    void updateItemOnValuesUpdate(const UpdateBlock& block,
                                   const std::shared_ptr<Entity>& entity);

    // Allows extending object-type handling without modifying handler dispatch.
    struct IObjectTypeHandler {
        virtual ~IObjectTypeHandler() = default;
        virtual void onCreate(const UpdateBlock& /*block*/, std::shared_ptr<Entity>& /*entity*/,
                              bool& /*newItemCreated*/) {}
        virtual void onValuesUpdate(const UpdateBlock& /*block*/, std::shared_ptr<Entity>& /*entity*/) {}
        virtual void onMovementUpdate(const UpdateBlock& /*block*/, std::shared_ptr<Entity>& /*entity*/) {}
    };
    struct UnitTypeHandler;
    struct PlayerTypeHandler;
    struct GameObjectTypeHandler;
    struct ItemTypeHandler;
    struct CorpseTypeHandler;
    std::unordered_map<uint8_t, std::unique_ptr<IObjectTypeHandler>> typeHandlers_;
    void initTypeHandlers();
    [[nodiscard]] IObjectTypeHandler* getTypeHandler(ObjectType type) const;

    void onCreateUnit(const UpdateBlock& block, std::shared_ptr<Entity>& entity);
    void onCreatePlayer(const UpdateBlock& block, std::shared_ptr<Entity>& entity);
    void onCreateGameObject(const UpdateBlock& block, std::shared_ptr<Entity>& entity);
    void onCreateItem(const UpdateBlock& block, bool& newItemCreated);
    void onCreateCorpse(const UpdateBlock& block, const Entity& corpse);
    void handleDisplayIdChange(const UpdateBlock& block,
                               const std::shared_ptr<Entity>& entity,
                               const std::shared_ptr<Unit>& unit,
                               const UnitFieldUpdateResult& result);
    void onValuesUpdateUnit(const UpdateBlock& block, std::shared_ptr<Entity>& entity);
    void onValuesUpdatePlayer(const UpdateBlock& block, std::shared_ptr<Entity>& entity);
    void onValuesUpdateItem(const UpdateBlock& block, std::shared_ptr<Entity>& entity);
    void onValuesUpdateGameObject(const UpdateBlock& block, std::shared_ptr<Entity>& entity);

    // Collects addon events during block processing, flushes at the end. The
    // owners a packet's fields are handed to raise theirs here too, so they
    // reach the interface in the order they happened.
    struct PendingEvents final : FrameXmlEvents {
        std::vector<std::pair<std::string, std::vector<std::string>>> events;
        void emit(const std::string& name, const std::vector<std::string>& args = {}) {
            events.emplace_back(name, args);
        }
        void clear() { events.clear(); }
        void fireFrameXmlEvent(const std::string& name, const std::vector<std::string>& args) override {
            emit(name, args);
        }
        // Queued: the flush drops them when no interface is listening.
        [[nodiscard]] bool frameXmlListening() const override { return true; }
    };
    PendingEvents pendingEvents_;
    void flushPendingEvents();
};

} // namespace game
} // namespace wowee
