#pragma once

#include "game/flat_field_map.hpp"
#include "game/world_packet_data.hpp"
#include "core/coordinates.hpp"
#include "game/opcodes.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace core { class EventBus; }
namespace game {

class GameData;

class ChatLog;
class EntityManager;
class FrameXmlEvents;
class PacketParsers;
class PacketSender;
class TransportManager;
struct AuraSlot;
enum class Race : uint8_t;
enum class WorldState;

/// What movement, taxis and transports ask of the rest of the game, and the
/// few things they make happen there. GameHandler implements it; several of
/// its answers it already gives other handlers under the same names.
class MovementQueries {
public:
    virtual ~MovementQueries() = default;

    // ---- The session and the world in view ----
    [[nodiscard]] virtual WorldState worldState() const = 0;
    [[nodiscard]] virtual uint64_t characterGuid() const = 0;
    /// The race of the character logged in, for the taxi mount's faction;
    /// Human before one is.
    [[nodiscard]] virtual Race characterRace() const = 0;
    [[nodiscard]] virtual uint32_t currentMapId() const = 0;
    [[nodiscard]] virtual const EntityManager& entities() const = 0;
    /// A game object that came into view as a transport.
    [[nodiscard]] virtual bool isTransport(uint64_t guid) const = 0;
    /// The transports' own motion. Never null in a running game.
    [[nodiscard]] virtual TransportManager* transports() = 0;
    [[nodiscard]] virtual PacketParsers& packetParsers() = 0;
    /// The taxi mounts' display ids, once the spawner has read them.
    [[nodiscard]] virtual uint32_t gryphonDisplayId() const = 0;
    [[nodiscard]] virtual uint32_t wyvernDisplayId() const = 0;

    // ---- The target and the cast ----
    [[nodiscard]] virtual uint64_t currentTargetGuid() const = 0;
    /// An accepted resurrection the server has yet to carry out.
    [[nodiscard]] virtual bool resurrectPending() const = 0;
    [[nodiscard]] virtual bool casting() const = 0;
    [[nodiscard]] virtual bool channeling() const = 0;
    /// Eating or drinking.
    [[nodiscard]] virtual bool restoring() const = 0;
    [[nodiscard]] virtual const std::vector<AuraSlot>& playerAuras() const = 0;
    [[nodiscard]] virtual uint32_t currentCastSpellId() const = 0;
    /// The spell the player last cast from the ground, not from a mount.
    [[nodiscard]] virtual uint32_t lastGroundCastSpellId() const = 0;
    /// The mount's aura is gone with the mount: take it off the buff bar.
    virtual void forgetPlayerAura(uint32_t spellId) = 0;
    virtual void cancelCast() = 0;
    virtual void resetCastState() = 0;

    // ---- The vehicle ----
    virtual void leftVehicle() = 0;

    // ---- Arriving somewhere ----
    /// SMSG_NEW_WORLD moved the player to `mapId`.
    virtual void arrivedOnMap(uint32_t mapId) = 0;
    /// Keep the world connection's packets for a while, under `label`.
    virtual void traceWorldPackets(std::chrono::seconds duration, const char* label) = 0;
    /// What a new map leaves behind: the units in view and their casts and
    /// auras, the fight, the world states, the gossip markers, the cast.
    virtual void tearDownWorldForTransfer(uint32_t mapId) = 0;
    /// SMSG_NEW_WORLD on the same map, answering an accepted resurrection:
    /// the death, the fight and the cast end.
    virtual void resurrectedInPlace() = 0;
    virtual void resetPingTimer() = 0;
    virtual void sendPing() = 0;
    virtual void closeGossip() = 0;
};

class MovementHandler {
public:
    /// NEW_WORLD is held at the packet boundary until the client ends its frame.
    [[nodiscard]] bool hasPendingWorldTransfer() const { return pendingNewWorld_.has_value(); }
    void applyPendingWorldTransfer();
    void cancelPendingWorldTransfer() { pendingNewWorld_.reset(); }
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    /// @param assets Null in a client without game data: no taxi path or
    ///        area trigger table is read.
    MovementHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                    ChatLog& chat, const GameData& data, MovementQueries& queries,
                    MovementInfo& movementInfo);

    void registerOpcodes(DispatchTable& table);

    /// A character is entering the world: not on the last one's mount.
    void beginCharacter() { mount_ = Mount{}; }

    // --- The player's mount ---
    [[nodiscard]] bool isMounted() const { return mount_.displayId != 0; }
    /// UNIT_FIELD_MOUNTDISPLAYID as the client holds it, 0 on foot.
    [[nodiscard]] uint32_t mountDisplayId() const { return mount_.displayId; }
    /// The aura the mount came with, as far as it could be told: what
    /// cancelling the mount cancels.
    [[nodiscard]] uint32_t mountAuraSpellId() const { return mount_.auraSpellId; }
    /// The spell cast was a mount's.
    void noteMountAura(uint32_t spellId) { mount_.auraSpellId = spellId; }
    /// UNIT_FIELD_MOUNTDISPLAYID on the player, at create or in an update.
    void playerMountFieldArrived(uint32_t displayId, const FlatFieldMap& blockFields,
                                 FrameXmlEvents& packetEvents);
    /// Entering the world, or a far teleport: on foot.
    void enteredWorld();
    /// Once a frame, off a taxi: the mount is what the player's field says.
    void reconcileMount();

    /// A game object template arrived. A transport's (MO_TRANSPORT) names
    /// the TaxiPathNode route it flies, which is the only one a zeppelin or
    /// a continent ship ever gets.
    void transportTemplateArrived(const GameObjectQueryResponseData& data);

    // --- Public API (delegated from GameHandler) ---

    void sendMovement(Opcode opcode);
    /// Stop client-directed motion before suspending control (e.g. a map load).
    void stopInputMovement();
    void setPosition(float x, float y, float z);
    void setOrientation(float orientation);
    void setMovementPitch(float radians) { movementInfo.pitch = radians; }
    void dismount();

    // Follow target (moved from GameHandler)
    void followTarget();
    void cancelFollow();
    /// Once a frame: refresh the followed unit's position and move toward it,
    /// or stop following when it is gone. A no-op when nothing is followed.
    void updateFollow(float deltaTime);
    // Movement toward the followed target's live position. Recomputes the
    // target position fresh every call (from the live entity, not a stored
    // point), so it doesn't need the long-distance Z-interpolation guard the
    // headless client's single-shot /movement/goto needs - there's never a
    // large stale remaining distance to interpolate across.
    void updateFollowMovement(float deltaTime);

    // Area trigger detection
    struct AreaTriggerEntry {
        uint32_t id = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
        float radius = 0;
        float boxLength = 0, boxWidth = 0, boxHeight = 0;
        float boxYaw = 0;
    };
    void loadAreaTriggerDbc();
    void checkAreaTriggers();
    /// Once a frame: count down the hold and check the triggers every
    /// AREA_TRIGGER_CHECK_INTERVAL.
    void updateAreaTriggers(float deltaTime);
    /// The player arrived on `mapId` - a login, a teleport, a transfer.
    /// Triggers are held for a while: the player can leave and re-enter one
    /// within seconds and be teleported again before the world has loaded.
    /// Deeprun Tram (map 369) is a narrow hallway with portal triggers close
    /// to the spawn, so its hold is short enough that the exits work.
    void holdAreaTriggersOnArrival(uint32_t mapId);
    /// AreaTrigger.dbc is read again on the next check.
    void forgetAreaTriggerTable();

    // Transport attachment
    struct TransportAttachment {
        ObjectType type = ObjectType::OBJECT;
        uint64_t transportGuid = 0;
        glm::vec3 localOffset{0.0f};
        float localOrientation = 0.0f;
        bool hasLocalOrientation = false;
        bool offsetNeedsTransportResolution = false;
    };
    /// The server's own word on where the player is: a MOVEMENT block
    /// (`movementUpdate`), which also turns the player and catches a client
    /// that has drifted far from it, or the movement part of the player's
    /// create block. Says whether the player rides a transport. Returns the
    /// player's world position when they ride one this client has placed,
    /// for the player's entity.
    std::optional<glm::vec3> serverPlacedPlayer(const UpdateBlock& block, const glm::vec3& canonicalPos,
                                                float canonicalOrientation, bool movementUpdate);

    // --- The player's transport ---
    [[nodiscard]] bool isOnTransport() const { return playerTransportGuid_ != 0; }
    [[nodiscard]] uint64_t getPlayerTransportGuid() const { return playerTransportGuid_; }
    [[nodiscard]] glm::vec3 getPlayerTransportOffset() const { return playerTransportOffset_; }
    /// The player was on `guid` a moment ago, or is: the hold that keeps a
    /// transport alive across the out-of-range flapping of its last rider.
    [[nodiscard]] bool recentlyAboard(uint64_t guid) const {
        return playerTransportStickyGuid_ == guid && playerTransportStickyTimer_ > 0.0f;
    }
    /// Compose the transport's transform with the player's local offset.
    glm::vec3 getComposedWorldPosition();
    /// Client-side transport board/disembark check by proximity to the live
    /// deck. Covers M2 trams/lifts and client-animated TaxiPathNode WMO ships.
    void updateM2TransportBoarding(const glm::vec3& playerCanonical);
    /// Board `transportGuid` at a canonical local offset - refused, and said,
    /// while the transport is not yet registered.
    void setPlayerOnTransport(uint64_t transportGuid, const glm::vec3& localOffset);
    void setPlayerTransportOffset(const glm::vec3& offset) { playerTransportOffset_ = offset; }
    void clearPlayerTransport();
    /// Once a frame: count the hold down.
    void updatePlayerTransportHold(float deltaTime);
    // Preserve an authoritative on-deck offset while a continent transfer
    // tears down the origin map's transport and constructs its destination
    // instance.
    void beginPlayerTransportWorldTransfer(uint32_t destinationMapId,
                                           const glm::vec3& localOffset);
    [[nodiscard]] bool hasPendingPlayerTransportWorldTransfer() const {
        return pendingPlayerTransportTransfer_;
    }
    bool completePlayerTransportWorldTransfer(uint64_t transportGuid,
                                              glm::vec3& worldPosition);

    [[nodiscard]] bool hasTransportAttachment(uint64_t guid) const {
        return transportAttachments_.count(guid) != 0;
    }
    void clearTransportAttachments() { transportAttachments_.clear(); }
    void setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                const glm::vec3& localOffset, bool hasLocalOrientation,
                                float localOrientation,
                                bool offsetNeedsTransportResolution = false);
    void clearTransportAttachment(uint64_t childGuid);
    void updateAttachedTransportChildren(float deltaTime);

    // Movement info accessors
    [[nodiscard]] const MovementInfo& getMovementInfo() const { return movementInfo; }
    MovementInfo& getMovementInfoMut() { return movementInfo; }

    // Speed accessors
    [[nodiscard]] float getServerRunSpeed() const { return serverRunSpeed_; }
    [[nodiscard]] float getServerWalkSpeed() const { return serverWalkSpeed_; }
    [[nodiscard]] float getServerSwimSpeed() const { return serverSwimSpeed_; }
    [[nodiscard]] float getServerSwimBackSpeed() const { return serverSwimBackSpeed_; }
    [[nodiscard]] float getServerFlightSpeed() const { return serverFlightSpeed_; }
    [[nodiscard]] float getServerFlightBackSpeed() const { return serverFlightBackSpeed_; }
    [[nodiscard]] float getServerRunBackSpeed() const { return serverRunBackSpeed_; }
    [[nodiscard]] float getServerTurnRate() const { return serverTurnRate_; }

    // CREATE_OBJECT/LIVING carries the authoritative speed table on login.
    // Apply it to the same cache used by camera movement and later force-speed
    // packets so reconnecting while mounted retains the server-provided speed.
    void applyServerMovementSpeeds(float walk, float run, float runBack,
                                   float swim, float swimBack, float flight,
                                   float flightBack, float turn, float pitch);

    // Movement flag queries
    [[nodiscard]] bool isPlayerRooted() const { return movementInfo.isPlayerRooted(); }
    [[nodiscard]] bool isGravityDisabled() const { return movementInfo.isGravityDisabled(); }
    [[nodiscard]] bool isFeatherFalling() const { return movementInfo.isFeatherFalling(); }
    [[nodiscard]] bool isWaterWalking() const { return movementInfo.isWaterWalking(); }
    [[nodiscard]] bool isPlayerFlying() const { return movementInfo.isPlayerFlying(); }
    [[nodiscard]] bool isHovering() const { return movementInfo.isHovering(); }
    [[nodiscard]] bool isSwimming() const { return movementInfo.isSwimming(); }

    // Taxi / Flight Paths
    [[nodiscard]] bool isTaxiWindowOpen() const { return taxiWindowOpen_; }
    void closeTaxi();
    void activateTaxi(uint32_t destNodeId);
    [[nodiscard]] bool isOnTaxiFlight() const { return onTaxiFlight_; }

    /// True while a locally-initiated dismount is waiting on the server to
    /// agree. The player's mount field keeps its old value for a few frames
    /// after the request, and taking that at face value re-mounts them.
    [[nodiscard]] bool isDismountPending() const { return dismountGraceRemaining_ > 0.0f; }
    void clearDismountPending() { dismountGraceRemaining_ = 0.0f; }
    [[nodiscard]] bool isTaxiMountActive() const { return taxiMountActive_; }
    [[nodiscard]] bool isTaxiActivationPending() const { return taxiActivatePending_; }
    void forceClearTaxiAndMovementState();
    [[nodiscard]] const std::string& getTaxiDestName() const { return taxiDestName_; }
    [[nodiscard]] const ShowTaxiNodesData& getTaxiData() const { return currentTaxiData_; }
    [[nodiscard]] uint32_t getTaxiCurrentNode() const { return currentTaxiData_.nearestNode; }

    struct TaxiNode {
        uint32_t id = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
        std::string name;
        uint32_t mountDisplayIdAlliance = 0;
        uint32_t mountDisplayIdHorde = 0;
    };
    struct TaxiPathEdge {
        uint32_t pathId = 0;
        uint32_t fromNode = 0, toNode = 0;
        uint32_t cost = 0;
    };
    struct TaxiPathNode {
        uint32_t id = 0;
        uint32_t pathId = 0;
        uint32_t nodeIndex = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
    };

    [[nodiscard]] const std::unordered_map<uint32_t, TaxiNode>& getTaxiNodes() const { return taxiNodes_; }

    /// Drops what was read out of the taxi DBCs, so the next ask reloads them.
    ///
    /// Called when the active expansion changes. GameHandler::resetDbcCaches
    /// cleared copies of its own for this, and getTaxiNodes forwards here - so
    /// switching expansion left the previous one's flight points in place and
    /// nothing said so.
    void resetTaxiDbcCache() {
        taxiNodes_.clear();
        taxiPathEdges_.clear();
        taxiPathNodes_.clear();
        taxiDbcLoaded_ = false;
    }
    // WotLK 3.3.5a TaxiNodes.dbc has 384 entries; the known-taxi bitmask
    // is 12 × uint32 = 384 bits. Node IDs outside this range are invalid.
    static constexpr uint32_t kMaxTaxiNodeId = 384;
    [[nodiscard]] bool isKnownTaxiNode(uint32_t nodeId) const {
        if (nodeId == 0 || nodeId > kMaxTaxiNodeId) return false;
        uint32_t idx = nodeId - 1;
        return (knownTaxiMask_[idx / 32] & (1u << (idx % 32))) != 0;
    }
    [[nodiscard]] uint32_t getTaxiCostTo(uint32_t destNodeId) const;
    /// True when the taxi cost map has a route from the current node to dest.
    [[nodiscard]] bool hasTaxiRouteTo(uint32_t destNodeId) const;
    /// Node-id hop chain current → dest (inclusive); empty if unreachable.
    [[nodiscard]] std::vector<uint32_t> getTaxiRouteTo(uint32_t destNodeId) const;
    [[nodiscard]] bool taxiNpcHasRoutes(uint64_t guid) const {
        auto it = taxiNpcHasRoutes_.find(guid);
        return it != taxiNpcHasRoutes_.end() && it->second;
    }

    void updateClientTaxi(float deltaTime);
    // Ends the active client-simulated taxi flight. See the definition in
    // movement_handler.cpp for what snapToFinalWaypoint means - callers outside
    // MovementHandler (SMSG_DISMOUNT / UNIT_FIELD_MOUNTDISPLAYID handlers reacting
    // to an authoritative server completion signal) should pass false.
    void finishClientTaxiFlight(bool snapToFinalWaypoint);
    // Remember an early server-side dismount/flag clear. Some cores send the
    // completion signal well before our local spline endpoint and do not repeat
    // it, so updateClientTaxi() consumes it once the player reaches the landing
    // zone instead of waiting for the exact final waypoint.
    void deferServerTaxiCompletion();
    // Server cores can clear taxi flags/dismount before the client spline reaches
    // its final waypoint. Only treat those signals as authoritative near arrival.
    [[nodiscard]] bool isNearTaxiDestination(float maxDistance = 24.0f) const;
    uint32_t nextMovementTimestampMs();
    void sanitizeMovementForTaxi();
    /// The player's create block has them on a taxi flight the client did not
    /// start - entering the world mid-flight. Shown as one: no walking flags,
    /// on the mount.
    void playerCreatedInFlight();

    // Heartbeat / movement timing (for GameHandler::update())
    float& timeSinceLastMoveHeartbeatRef() { return timeSinceLastMoveHeartbeat_; }
    [[nodiscard]] float getMoveHeartbeatInterval() const { return moveHeartbeatInterval_; }
    [[nodiscard]] bool isServerMovementAllowed() const { return serverMovementAllowed_; }
    /// Whether the player is in the air on a flying mount, as the movement
    /// code worked it out - which sets and clears FLYING, the flag the server
    /// reads flight from. Only while flight is allowed: a FLYING the server
    /// set by itself is left alone.
    void setFlightAirborne(bool airborne);
    /// Where the last movement packet that went out put the player, in
    /// canonical coordinates - which is where the server has them - and false
    /// before any has.
    [[nodiscard]] bool lastSentPosition(glm::vec3& out) const {
        if (!hasSentMovement_) return false;
        out = lastSentPos_;
        return true;
    }
    void setServerMovementAllowed(bool v) { serverMovementAllowed_ = v; }
    uint32_t& monsterMovePacketsThisTickRef() { return monsterMovePacketsThisTick_; }
    uint32_t& monsterMovePacketsDroppedThisTickRef() { return monsterMovePacketsDroppedThisTick_; }

    // Movement clock / fall state setters (formerly accessed via friend)
    void resetMovementClock() { movementClockStart_ = std::chrono::steady_clock::now(); lastMovementTimestampMs_ = 0; }
    void setFalling(bool falling) { isFalling_ = falling; }
    void setFallStartMs(uint32_t ms) { fallStartMs_ = ms; }

    [[nodiscard]] uint64_t getTaxiNpcGuid() const { return taxiNpcGuid_; }

    // Methods also called from GameHandler's registerOpcodeHandlers
    void handleCompressedMoves(network::Packet& packet);
    void handleForceMoveFlagChange(network::Packet& packet, const char* name, Opcode ackOpcode, uint32_t flag, bool set);
    void handleMoveSetCollisionHeight(network::Packet& packet);
    void applyTaxiMountForCurrentNode();

private:
    //     Track player-on-transport state from movement blocks: the CREATE and
    //     MOVEMENT paths share it. With updateMovementInfoPos (MOVEMENT) the
    //     player's position is taken from the block when they are not on a
    //     placed transport; without it (CREATE) only a placed transport's
    //     composed position is.
    std::optional<glm::vec3> applyPlayerTransportState(const UpdateBlock& block, const glm::vec3& canonicalPos,
                                                       bool updateMovementInfoPos);
    void resyncPlayerIfFarFromServer(const glm::vec3& serverCanonicalPos);
    void handleDismount(network::Packet& packet);
    void handleMountResult(network::Packet& packet);
    void handleDismountResult(network::Packet& packet);

private:
    // --- Packet handlers ---
    void handleMonsterMove(network::Packet& packet);
    void handleMonsterMoveTransport(network::Packet& packet);
    void handleOtherPlayerMovement(network::Packet& packet);
    void handleMoveSetSpeed(network::Packet& packet);
    /// Set one of another unit's movement speeds.
    void applyUnitSpeed(uint64_t guid, float MovementSpeeds::* speed, float value);
    void handleForceRunSpeedChange(network::Packet& packet);
    network::Packet buildForceAck(Opcode ackOpcode, uint32_t counter, float value = 0);
    /// A forced speed change. Ours is applied and acknowledged; another
    /// unit's goes into its speeds as unitSpeed (null: not one it keeps).
    void handleForceSpeedChange(network::Packet& packet, const char* name, Opcode ackOpcode,
                                float* speedStorage, float MovementSpeeds::* unitSpeed);
    void handleForceMoveRootState(network::Packet& packet, bool rooted);
    void handleMoveKnockBack(network::Packet& packet);
    void handleTeleportAck(network::Packet& packet);
    void handleNewWorld(network::Packet& packet);
    void applyNewWorld(network::Packet& packet);
    std::optional<network::Packet> pendingNewWorld_;
    void handleShowTaxiNodes(network::Packet& packet);
    void handleClientControlUpdate(network::Packet& packet);
    void handleActivateTaxiReply(network::Packet& packet);
    void loadTaxiDbc();

    // --- Private helpers ---
    void buildTaxiCostMap();
    void startClientTaxiPath(const std::vector<uint32_t>& pathNodes);
    // Evaluates the same uniform Catmull-Rom curve used to render/pace taxi
    // flight motion, shared between the per-segment arc-length precomputation
    // in startClientTaxiPath() and the per-frame position update.
    static glm::vec3 evalTaxiCatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                         const glm::vec3& p2, const glm::vec3& p3, float t);
    // Commits a taxi flight already built by startClientTaxiPath(): snaps the player
    // to the path start and sets taxiClientActive_. Only called once
    // SMSG_ACTIVATETAXIREPLY confirms success - see activateTaxi()'s comment.
    void beginTaxiFlightMotion();
    bool restoreWorldTransferFallbackIfNearOrigin(const char* context);

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    const GameData& data_;
    MovementQueries& queries_;

    // --- Movement state ---
    // Reference to GameHandler's movementInfo to avoid desync
    MovementInfo& movementInfo;
    std::chrono::steady_clock::time_point movementClockStart_ = std::chrono::steady_clock::now();
    uint32_t lastMovementTimestampMs_ = 0;
    bool serverMovementAllowed_ = true;
    uint32_t monsterMovePacketsThisTick_ = 0;
    uint32_t monsterMovePacketsDroppedThisTick_ = 0;

    // Fall/jump tracking
    bool isFalling_ = false;
    uint32_t fallStartMs_ = 0;

    // Heartbeat timing
    int heartbeatLogCount_ = 0;  // periodic position audit counter

    bool areaTriggerDbcLoaded_ = false;
    std::vector<AreaTriggerEntry> areaTriggers_;
    std::unordered_set<uint32_t> activeAreaTriggers_;  // triggers the player is inside
    float areaTriggerCheckTimer_ = 0.0f;
    bool areaTriggerSuppressFirst_ = false;  // suppress the first check after a transfer
    float areaTriggerCooldown_ = 0.0f;       // seconds left in which no trigger fires

    uint64_t followTargetGuid_ = 0;
    glm::vec3 followRenderPos_{0.0f};  // render-space position of the followed unit, each frame

    /// Units and objects riding a transport, by guid.
    std::unordered_map<uint64_t, TransportAttachment> transportAttachments_;

    // The player's transport.
    uint64_t playerTransportGuid_ = 0;                  // the transport ridden (0 = none)
    glm::vec3 playerTransportOffset_ = glm::vec3(0.0f); // the player's offset on it
    uint64_t playerTransportStickyGuid_ = 0;            // the last one ridden (temporary retention)
    float playerTransportStickyTimer_ = 0.0f;           // seconds to keep it after transient clears
    // Consecutive frames a ship rider has had no deck beneath them. Walking off
    // onto the pier leaves the rider well inside the ship's boarding footprint,
    // so losing the deck is what actually distinguishes "ashore" from "aboard".
    // Counted rather than tested instantly so a jump is not a disembark.
    int shipNoDeckSupportFrames_ = 0;
    bool pendingPlayerTransportTransfer_ = false;
    uint64_t pendingPlayerTransportGuid_ = 0;
    uint32_t pendingPlayerTransportEntry_ = 0;
    uint32_t pendingPlayerTransportMapId_ = 0xFFFFFFFFu;
    glm::vec3 pendingPlayerTransportOffset_ = glm::vec3(0.0f);
    uint32_t lastAreaTriggerId_ = 0;
    uint32_t postTransferReturnAreaTriggerId_ = 0;
    // Previous checkAreaTriggers() sample for swept-path testing (canonical
    // coords). Mounted speed covers ~3 yd per 0.25s check, enough to straddle
    // small portal boxes when only the instantaneous position is tested.
    glm::vec3 lastAreaTriggerCheckPos_{0.0f};
    uint32_t lastAreaTriggerCheckMapId_ = 0xFFFFFFFFu;
    bool lastAreaTriggerCheckValid_ = false;
    bool postTransferReturnAreaTriggerSawNear_ = false;
    bool pendingAreaTriggerDestinationValid_ = false;
    uint32_t pendingAreaTriggerDestinationMapId_ = 0;
    glm::vec3 pendingAreaTriggerDestinationServerPos_{0.0f};
    float pendingAreaTriggerDestinationServerO_ = 0.0f;
    bool worldTransferFallbackValid_ = false;
    uint32_t worldTransferFallbackMapId_ = 0;
    uint32_t worldTransferFallbackTriggerId_ = 0;
    glm::vec3 worldTransferFallbackCanonicalPos_{0.0f};
    float worldTransferFallbackCanonicalO_ = 0.0f;
    float timeSinceLastMoveHeartbeat_ = 0.0f;
    // Seconds left in which a stale non-zero mount field is not to be believed.
    // Bounded so a dismount the server refuses recovers on its own rather than
    // leaving the player permanently unable to look mounted.
    float dismountGraceRemaining_ = 0.0f;
    float moveHeartbeatInterval_ = 0.5f;
    uint32_t lastHeartbeatSendTimeMs_ = 0;
    float lastHeartbeatX_ = 0.0f;
    float lastHeartbeatY_ = 0.0f;
    float lastHeartbeatZ_ = 0.0f;
    uint32_t lastHeartbeatFlags_ = 0;
    uint64_t lastHeartbeatTransportGuid_ = 0;
    uint32_t lastNonHeartbeatMoveSendTimeMs_ = 0;
    glm::vec3 lastSentPos_{0.0f};
    bool hasSentMovement_ = false;
    /// Why movement is being held back from the server, said once per stretch
    /// of it. See sendMovement.
    const char* movementHeldReason_ = nullptr;
    uint32_t lastFacingSendTimeMs_ = 0;
    float lastFacingSentOrientation_ = 0.0f;

    // Speed state
    float serverRunSpeed_ = 7.0f;
    float serverWalkSpeed_ = 2.5f;
    float serverRunBackSpeed_ = 4.5f;
    float serverSwimSpeed_ = 4.722f;
    float serverSwimBackSpeed_ = 2.5f;
    float serverFlightSpeed_ = 7.0f;
    float serverFlightBackSpeed_ = 4.5f;
    float serverTurnRate_ = core::coords::PI;
    float serverPitchRate_ = core::coords::PI;

    // --- Taxi / Flight Path state ---
    std::unordered_map<uint64_t, bool> taxiNpcHasRoutes_;
    std::unordered_map<uint32_t, TaxiNode> taxiNodes_;
    std::vector<TaxiPathEdge> taxiPathEdges_;
    std::unordered_map<uint32_t, std::vector<TaxiPathNode>> taxiPathNodes_;
    bool taxiDbcLoaded_ = false;
    bool taxiWindowOpen_ = false;
    ShowTaxiNodesData currentTaxiData_;
    uint64_t taxiNpcGuid_ = 0;
    /// Triggers already reported as a near miss, so the log says it once.
    std::set<uint32_t> nearMissLogged_;
    bool onTaxiFlight_ = false;
    std::string taxiDestName_;
    // Set in activateTaxi(); used by finishClientTaxiFlight() to snap to the
    // destination TaxiNodes.dbc entry's own registered position rather than
    // taxiClientPath_'s last waypoint (from the separate TaxiPathNode.dbc
    // table) - live-confirmed the two can disagree by 100+ yards, since a
    // taxi path's own waypoint trace doesn't necessarily terminate exactly on
    // the node's registered coordinate the way GM teleports/.gps do.
    uint32_t taxiDestNodeId_ = 0;
    /// The player's own mount, assigned whole by beginCharacter.
    struct Mount {
        uint32_t displayId = 0;
        uint32_t auraSpellId = 0;  // what CMSG_CANCEL_AURA cancels where there is no CMSG_CANCEL_MOUNT_AURA
    };
    Mount mount_;
    bool taxiMountActive_ = false;
    bool taxiActivatePending_ = false;
    float taxiActivateTimer_ = 0.0f;
    // How long to wait for SMSG_ACTIVATETAXIREPLY before giving up - see
    // updateClientTaxi()'s comment on why a dropped reply needs this now.
    static constexpr float kTaxiActivateReplyTimeoutSeconds = 8.0f;
    bool taxiClientActive_ = false;
    size_t taxiClientIndex_ = 0;
    std::vector<glm::vec3> taxiClientPath_;
    // Arc length of the smooth Catmull-Rom curve actually rendered through each
    // consecutive pair of taxiClientPath_ points - one entry per segment, same
    // indexing as taxiClientIndex_. Pacing (how long each segment takes to
    // traverse at taxiClientSpeed_) uses this instead of the straight-line
    // chord distance between waypoints: the server's own flight spline paces
    // by the curve's real length too, and a smooth curve through a series of
    // waypoints is essentially always shorter than the straight-line polyline
    // connecting the same points (the curve "cuts" corners at turns). Using
    // chord length there made our total flight duration systematically longer
    // than the server's identical-speed (32.0f) spline - unnoticeable on a
    // short hop but live-confirmed to drift ~90 yards short of the true
    // destination on a long multi-waypoint flight (Booty Bay -> Stormwind),
    // by the time the server's own flight ends and clears UNIT_FLAG_TAXI_FLIGHT.
    // Computed once per segment when the path is built (see startClientTaxiPath()).
    std::vector<float> taxiClientSegmentArcLengths_;
    float taxiClientSpeed_ = 32.0f;
    float taxiClientSegmentProgress_ = 0.0f;
    bool taxiServerCompletionPending_ = false;
    uint32_t knownTaxiMask_[12] = {};
    bool taxiMaskInitialized_ = false;
    std::unordered_map<uint32_t, uint32_t> taxiCostMap_;
    // BFS predecessor per node from buildTaxiCostMap; getTaxiRouteTo walks it.
    std::unordered_map<uint32_t, uint32_t> taxiPrevMap_;

    bool followMoveMoving_ = false;  // whether MSG_MOVE_START_FORWARD has been sent for the current follow
};

} // namespace game
} // namespace wowee
