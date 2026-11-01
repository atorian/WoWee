#pragma once

#include "game/addon_event_queue.hpp"

#include "game/quest_giver_status.hpp"
#include "game/craft_recipe.hpp"
#include "game/game_interfaces.hpp"
#include "game/action_bar_handler.hpp"
#include "game/chat_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/pet_handler.hpp"
#include "game/frame_xml_events.hpp"
#include "game/packet_sender.hpp"
#include "game/world_packet_data.hpp"
#include "game/character.hpp"
#include "game/opcodes.hpp"
#include "game/update_field_table.hpp"
#include "game/inventory.hpp"
#include "game/spell_defines.hpp"
#include "game/group_defines.hpp"
#include "game/handler_types.hpp"
#include "game/combat_handler.hpp"
#include "game/player_handler.hpp"
#include "game/query_cache_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/reputation_handler.hpp"
#include "game/social_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/warden_session.hpp"
#include "game/world_state.hpp"
#include "game/world_handler.hpp"
#include "game/game_services.hpp"
#include "game/world_login.hpp"
#include "network/packet.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <array>
#include <functional>
#include <cstdint>
#include <utility>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "game/spell_description_eval.hpp"
#include <map>
#include <optional>
#include <algorithm>
#include <chrono>
#include <future>

namespace wowee::game {
    class TransportManager;
    class WardenCrypto;
    class WardenMemory;
    class WardenModule;
    class PacketParsers;
    class ChatHandler;
    class MovementHandler;
    class InventoryHandler;
    class SocialHandler;
    class WardenHandler;
    class AccountDataHandler;
    class RealmTimeHandler;
    class WeatherHandler;
}

namespace wowee {
namespace network { class WorldConnection; class Packet; }
namespace core { class EventBus; class CVarStore; }

namespace game {

struct EzWowPatch;

/**
 * Quest giver status values (WoW 3.3.5a)
 */
/**
 * World connection callbacks
 */
using WorldConnectSuccessCallback = std::function<void()>;
using WorldConnectFailureCallback = std::function<void(const std::string& reason)>;

/**
 * GameHandler - Manages world server connection and game protocol
 *
 * Handles:
 * - Connection to world server
 * - Authentication with session key from auth server
 * - RC4 header encryption
 * - Character enumeration
 * - World entry
 * - Game packets
 */
class GameHandler : public IConnectionState,
                     public QueryCacheQueries,
                     public WorldQueries,
                     public PacketSender,
                     public FrameXmlEvents,
                     public ActionBarQueries,
                     public InventoryQueries,
                     public PetQueries,
                     public SpellQueries,
                     public ChatQueries,
                     public QuestQueries,
                     public ReputationQueries,
                     public SocialQueries,
                     public WardenSession,
                     public CombatQueries,
                     public MovementQueries,
                     public PlayerQueries {
public:
    // Talent data structures (aliased from handler_types.hpp)
    using TalentEntry = game::TalentEntry;
    using TalentTabEntry = game::TalentTabEntry;

    explicit GameHandler(GameServices& services);
    ~GameHandler() override;

    const GameServices& services() const { return services_; }
    /// Where this announces what happened; see game/game_events.hpp.
    core::EventBus& eventBus() const { return *services_.events; }
    /// The emotes' commands, text and animations.
    const EmoteTable& emotes() const { return *services_.emotes; }
    /// The CVars, as the player and the interface last set them.
    const core::CVarStore& cvars() const { return *services_.cvars; }

    /** Access the active opcode table (wire ↔ logical mapping). */
    const OpcodeTable& getOpcodeTable() const { return opcodeTable_; }
    OpcodeTable& getOpcodeTable() { return opcodeTable_; }
    const UpdateFieldTable& getUpdateFieldTable() const { return updateFieldTable_; }
    UpdateFieldTable& getUpdateFieldTable() { return updateFieldTable_; }
    PacketParsers* getPacketParsers() { return packetParsers_.get(); }
    void setPacketParsers(std::unique_ptr<PacketParsers> parsers);

    /**
     * Connect to world server
     *
     * @param host World server hostname/IP
     * @param port World server port (default 8085)
     * @param sessionKey 40-byte session key from auth server
     * @param accountName Account name (will be uppercased)
     * @param build Client build number (default 12340 for 3.3.5a)
     * @return true if connection initiated
     */
    bool connect(const std::string& host,
                 uint16_t port,
                 const std::vector<uint8_t>& sessionKey,
                 const std::string& accountName,
                 uint32_t build = 12340,
                 uint32_t realmId = 0,
                 const std::string& extensionServerHost = {},
                 uint16_t extensionServerPort = 3724);
    /// The same, from what a login gave: a new connection, a new Warden
    /// session and the handshake from the start.
    bool connect(const WorldLogin& login);
    /// The last login made, to make it again once the connection is lost.
    [[nodiscard]] WorldLogin lastWorldLogin() const;

    /**
     * Disconnect from world server
     */
    void disconnect();

    /// Whether the connection was lost while the character was in the world
    /// and the world has not been entered again since. The client is then
    /// reconnecting, the character still standing in the world it drew.
    [[nodiscard]] bool worldConnectionLost() const { return worldConnectionLost_; }

    /// Out of the world and back to the character list, on the same connection.
    ///
    /// What a logout is in the original client: the server has said
    /// SMSG_LOGOUT_COMPLETE and keeps the session, so the world is cleared and
    /// the character list asked for again. The socket and the Warden session
    /// stay as they are - a reconnect would start a new session the server
    /// may not grant while the old one is still closing.
    void returnToCharacterSelect();

    /**
     * Check if connected to world server
     */
    bool isConnected() const override;
    bool isInWorld() const override { return state == WorldState::IN_WORLD && socket; }

    // What the domain handlers are handed instead of this class: the world
    // connection to send on (PacketSender), the FrameXML event channel
    // (FrameXmlEvents) and what the action bar and the inventory ask of the
    // rest of the game (ActionBarQueries, InventoryQueries). See AGENTS.md,
    // "A new domain handler".
    bool connected() const override { return isConnected(); }
    // MovementQueries (the rest it shares with the other handlers' queries)
    WorldState worldState() const override { return state; }
    Race characterRace() const override { return playerRace_; }
    bool isTransport(uint64_t guid) const override { return worldHandler_->isTransportGuid(guid); }
    TransportManager* transports() override { return transportManager_.get(); }
    uint32_t gryphonDisplayId() const override { return services_.gryphonDisplayId; }
    uint32_t wyvernDisplayId() const override { return services_.wyvernDisplayId; }
    bool resurrectPending() const override { return combatHandler_->resurrectPending(); }
    bool channeling() const override { return spellHandler_->isChanneling(); }
    const std::vector<AuraSlot>& playerAuras() const override { return spellHandler_->getPlayerAuras(); }
    void resetCastState() override { spellHandler_->resetCastState(); }
    uint32_t lastGroundCastSpellId() const override { return spellHandler_->getLastGroundCastSpellId(); }
    void forgetPlayerAura(uint32_t spellId) override { spellHandler_->forgetPlayerAura(spellId); }
    void leftVehicle() override { vehicleId_ = 0; }
    void arrivedOnMap(uint32_t mapId) override { currentMapId_ = mapId; }
    void traceWorldPackets(std::chrono::seconds duration, const char* label) override;
    void tearDownWorldForTransfer(uint32_t mapId) override;
    void resurrectedInPlace() override;
    void resetPingTimer() override { timeSinceLastPing = 0.0f; }
    // CombatQueries (the rest it shares with the other handlers' queries)
    bool inInstance() const override { return isInInstance(); }
    bool corpseHoldsLoot(uint64_t guid) const override;
    uint8_t rangedWeaponInventoryType() const override;
    bool taxiMountActive() const override { return movementHandler_->isTaxiMountActive(); }
    void forgetTargetAuras() override { spellHandler_->clearTargetAuras(); }
    // WardenSession
    uint32_t clientBuild() const override { return build; }
    const std::vector<uint8_t>& loginSessionKey() const override { return sessionKey; }
    void serverMoneyReported(uint64_t copper) override;
    void knownTitlesReported(const KnownTitleUpdate& update) override;
    bool characterInWorld() const override { return isInWorld(); }
    /// Refused with "No connection to the server." while worldConnectionLost().
    bool worldCommandAllowed(const char* command) override;
    void send(const network::Packet& packet) override;
    void fireFrameXmlEvent(const std::string& name,
                           const std::vector<std::string>& args) override {
        fireAddonEvent(name, args);
    }
    bool frameXmlListening() const override { return hasAddonEventListener(); }
    ActionCooldown spellCooldown(uint32_t spellId) const override;
    ActionCooldown itemCooldown(uint32_t itemId) const override;
    void requestItemInfo(uint32_t itemId) override { inventoryHandler_->queryItemInfo(itemId, 0); }
    std::optional<uint32_t> playerFieldBytes() const override;

    // InventoryQueries
    uint64_t characterGuid() const override { return getPlayerGuid(); }
    uint64_t currentTargetGuid() const override { return combatHandler_->getTargetGuid(); }
    const EntityManager& entities() const override { return getEntityManager(); }
    const FlatFieldMap& playerFields() const override { return playerHandler_->fields(); }
    PacketParsers& packetParsers() override { return *packetParsers_; }
    std::string playerName(uint64_t guid) const override { return queryCacheHandler_->lookupName(guid); }
    void requestPlayerName(uint64_t guid) override { queryCacheHandler_->queryPlayerName(guid); }
    bool inGuild() const override;
    std::string spellName(uint32_t spellId) const override { return spellHandler_->getSpellName(spellId); }
    bool spellKnownToClient(uint32_t spellId) const override { return spellHandler_->isSpellKnownToClient(spellId); }
    uint32_t spellImplicitTargetA(uint32_t spellId) const override { return spellHandler_->getSpellImplicitTargetA(spellId); }
    uint32_t spellTargetFlags(uint32_t spellId) const override { return spellHandler_->getSpellTargetFlags(spellId); }
    std::array<uint32_t, 3> spellEffects(uint32_t spellId) const override;
    uint32_t classSkillLineOf(uint32_t spellId) override;
    std::string skillLineName(uint32_t skillLineId) const override { return spellHandler_->getSkillLineName(skillLineId); }
    std::string itemIconPath(uint32_t displayInfoId) const override { return getItemIconPath(displayInfoId); }
    std::string randomPropertyName(int32_t id) const override { return getRandomPropertyName(id); }
    std::vector<RandomStatBonus> randomStatBonuses(int32_t id, uint32_t suffixFactor) const override {
        return getRandomStatBonuses(id, suffixFactor);
    }
    std::string enchantName(uint32_t enchantId) const override { return spellHandler_->getEnchantName(enchantId); }
    bool casting() const override { return spellHandler_->isCasting(); }
    uint32_t currentCastSpellId() const override { return spellHandler_->getCurrentCastSpellId(); }
    bool restoring() const override { return spellHandler_->isRestoring(); }
    bool craftingWindowOpen() const override { return spellHandler_->isCraftingWindowOpen(); }
    bool mounted() const override { return movementHandler_->isMounted(); }
    uint32_t mountAuraSpellId() const override { return movementHandler_->mountAuraSpellId(); }
    bool carriesAura(uint32_t spellId) const override { return playerCarriesAura(spellId); }
    void noteItemCastSpell(uint32_t spellId) override;
    void stopMovingForCast() override;
    bool gameObjectLootPending(uint64_t lootGuid) const override;
    bool gameObjectLootOpened(uint64_t lootGuid) override;
    bool npcDialogOpen() const override;
    bool questWantsItem(uint32_t itemId) const override;
    bool actionBarHoldsItem() const override;
    void coinageFieldFoundAt(uint16_t index) override;
    // cancelCast, dismount, closeGossip and runInterfaceCommand are
    // InventoryQueries' too, declared with the rest of their kind below.

    // PetQueries (spellName is InventoryQueries' too)
    bool nameUnitInView(uint64_t guid, const std::string& name) override;

    // SpellQueries. The ones InventoryQueries shares (characterGuid,
    // entities, packetParsers, mounted, ...) are above; the ones this class
    // already had under the same name (sendMovement, setStandState,
    // startAutoAttack, lootTarget, addCombatText, ...) are marked override
    // where they are declared.
    uint64_t focusTargetGuid() const override { return combatHandler_->getFocusGuid(); }
    uint64_t petGuid() const override;
    pet::IPetFeed* petFeed() override;
    std::string unitIdFor(uint64_t guid) const override { return guidToUnitId(guid); }
    uint32_t playerLevel() const override { return playerHandler_->level(); }
    const GameObjectQueryResponseData* gameObjectInfo(uint32_t entry) const override {
        return queryCacheHandler_->getCachedGameObjectInfo(entry);
    }
    bool friendlyFaction(uint32_t factionTemplateId) const override {
        return isFriendlyFaction(factionTemplateId);
    }
    bool aggressiveTowardPlayer(uint64_t guid) const override { return combatHandler_->isAggressiveTowardPlayer(guid); }
    const MovementInfo& playerMovement() const override { return movementInfo; }
    void setFacing(float canonicalYaw) override;
    void faceTowards(float canonicalYaw) override { faceCanonicalYaw(canonicalYaw); }
    float faceCamera() override { return faceCameraDirection(); }
    bool autoFaceTarget() const override { return isAutoFaceTarget(); }
    bool autoSelfCast() const override { return isAutoSelfCast(); }
    bool flying() const override { return isPlayerFlying(); }
    uint32_t mountDisplayId() const override { return movementHandler_->mountDisplayId(); }
    void noteMountAura(uint32_t spellId) override { movementHandler_->noteMountAura(spellId); }
    bool autoAttacking() const override { return combatHandler_->isAutoAttacking(); }
    bool inCombat() const override { return combatHandler_->isInCombat(); }
    const ItemQueryResponseData* itemInfo(uint32_t itemId) const override { return inventoryHandler_->getItemInfo(itemId); }
    const std::unordered_map<uint32_t, ItemQueryResponseData>& itemTemplates() const override {
        return inventoryHandler_->getItemInfoCache();
    }
    uint32_t itemEntryByGuid(uint64_t itemGuid) const override;
    uint32_t itemCountInBags(uint32_t itemId) const override { return inventoryHandler_->items().countItem(itemId); }
    bool awaitingItemTarget() const override { return inventoryHandler_->isAwaitingItemTarget(); }
    uint64_t pendingGameObjectInteraction() const override { return pendingGameObjectInteractGuid_; }
    uint64_t lastInteractedGameObject() const override { return lastInteractedGoGuid_; }
    void clearPendingGameObjectInteraction() override { pendingGameObjectInteractGuid_ = 0; }
    void clearLastInteractedGameObject() override { lastInteractedGoGuid_ = 0; }
    std::optional<HomeBind> homeBind() const override;
    std::string achievementName(uint32_t achievementId) override;
    achievement::IAchievementsFeed* achievementsFeed() override;
    SpellDataInfo spellData(uint32_t spellId) const override { return getSpellData(spellId); }
    std::string iconPath(uint32_t iconId) const override { return getIconPath(iconId); }

    // ChatQueries (characterGuid, playerName, requestPlayerName,
    // packetParsers, playerMovement and achievementName are above).
    const Character* activeCharacter() const override { return getActiveCharacter(); }
    uint32_t currentMapId() const override { return currentMapId_; }
    uint8_t playerClassOf(uint64_t guid) const override { return queryCacheHandler_->lookupPlayerClass(guid); }
    uint8_t playerRaceOf(uint64_t guid) const override { return queryCacheHandler_->lookupPlayerRace(guid); }
    uint8_t playerGenderOf(uint64_t guid) const override { return queryCacheHandler_->lookupPlayerGender(guid); }
    const GroupListData& party() const override;
    const GuildRosterData& guildRoster() const override;
    std::optional<uint32_t> guildRankRights() const override;
    std::string languageName(uint32_t languageId) const override { return getLanguageName(languageId); }

    // QuestQueries (the shared ones are above; closeVendor is declared with
    // the vendor's facade).
    bool inGroup() const override;
    void setResting(bool resting) override { playerHandler_->restStateArrived(resting); }
    std::string creatureName(uint32_t entry) const override { return queryCacheHandler_->getCachedCreatureName(entry); }
    void requestCreatureInfo(uint32_t entry) override { queryCacheHandler_->queryCreatureInfo(entry, 0); }
    void requestGameObjectInfo(uint32_t entry) override { queryCacheHandler_->queryGameObjectInfo(entry, 0); }
    void requestGameObjectInfo(uint32_t entry, uint64_t guid) override {
        queryCacheHandler_->queryGameObjectInfo(entry, guid);
    }
    void vendorCanRepair() override { inventoryHandler_->setVendorCanRepair(true); }
    void openedStableMaster(uint64_t npcGuid) override;

    // SocialQueries (the ones shared with the other handlers are above).
    bool battlegroundMap(uint32_t mapId) override { return isBattlegroundMap(mapId); }
    std::string mapName(uint32_t mapId) const override { return getMapName(mapId); }
    std::string areaName(uint32_t areaId) const override { return getAreaName(areaId); }
    std::string lfgDungeonName(uint32_t dungeonId) const override { return getLfgDungeonName(dungeonId); }
    uint8_t spellDispelType(uint32_t spellId) const override { return spellHandler_->getSpellDispelType(spellId); }
    const std::unordered_map<uint32_t, TalentTabEntry>& talentTabs() override {
        spellHandler_->loadTalentDbc();
        return spellHandler_->getAllTalentTabs();
    }
    const std::unordered_map<uint32_t, TalentEntry>& talents() override {
        spellHandler_->loadTalentDbc();
        return spellHandler_->getAllTalents();
    }
    void mirrorAurasByGuid(uint64_t guid, const std::vector<AuraSlot>& auras) override {
        spellHandler_->mirrorAurasByGuid(guid, auras);
    }
    const std::array<uint32_t, 19>* inspectedItemEntries(uint64_t guid) const override {
        return inventoryHandler_->inspectedItemEntries(guid);
    }
    bool showingHelm() const override { return playerHandler_->helmVisible(); }
    bool showingCloak() const override { return playerHandler_->cloakVisible(); }
    void setShowingHelm(bool shown) override { playerHandler_->showHelm(shown); }
    void setShowingCloak(bool shown) override { playerHandler_->showCloak(shown); }

    // PlayerQueries (markOnlineEquipmentDirty is the inventory's, below)
    Character* listedCharacter() override;
    void moneyFieldArrived(uint32_t copper, FrameXmlEvents& packetEvents) override;
    void bankBagSlotsArrived(uint8_t slots) override;
    void chosenTitleArrived(int32_t title) override;
    void playerFlagsArrived(uint32_t flags, FrameXmlEvents& packetEvents) override;

    // QueryCacheQueries (packetParsers and entities are above)
    void playerNameArrived(uint64_t guid, const std::string& name) override;
    void creatureTemplateArrived(const CreatureQueryResponseData& data) override;
    void gameObjectTemplateArrived(const GameObjectQueryResponseData& data) override;

    // WorldQueries. The session's, the caches' and some of the fight's
    // answers are above, under the names other handlers ask them by; these
    // hand the rest to their owners.
    bool hostileFaction(uint32_t factionTemplateId) const override { return isHostileFaction(factionTemplateId); }
    uint64_t movementTransportGuid() const override { return movementInfo.transportGuid; }
    uint64_t playerTransportGuid() const override { return movementHandler_->getPlayerTransportGuid(); }
    bool recentlyAboard(uint64_t guid) const override { return movementHandler_->recentlyAboard(guid); }
    void clearPlayerTransport() override { movementHandler_->clearPlayerTransport(); }
    void setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                const glm::vec3& localOffset, bool hasLocalOrientation,
                                float localOrientation, bool offsetNeedsTransportResolution) override {
        movementHandler_->setTransportAttachment(childGuid, type, transportGuid, localOffset,
                                                 hasLocalOrientation, localOrientation,
                                                 offsetNeedsTransportResolution);
    }
    void clearTransportAttachment(uint64_t childGuid) override { movementHandler_->clearTransportAttachment(childGuid); }
    std::optional<glm::vec3> serverPlacedPlayer(const UpdateBlock& block, const glm::vec3& canonicalPos,
                                                float canonicalOrientation, bool movementUpdate) override {
        return movementHandler_->serverPlacedPlayer(block, canonicalPos, canonicalOrientation, movementUpdate);
    }
    void applyServerMovementSpeeds(float walk, float run, float runBack, float swim, float swimBack,
                                   float flight, float flightBack, float turn, float pitch) override {
        movementHandler_->applyServerMovementSpeeds(walk, run, runBack, swim, swimBack, flight,
                                                    flightBack, turn, pitch);
    }
    void playerCreatedInFlight() override { movementHandler_->playerCreatedInFlight(); }
    void playerMountFieldArrived(uint32_t displayId, const FlatFieldMap& blockFields,
                                 FrameXmlEvents& packetEvents) override {
        movementHandler_->playerMountFieldArrived(displayId, blockFields, packetEvents);
    }
    void playerFieldsCreated(const FlatFieldMap& fields) override { playerHandler_->fieldsCreated(fields); }
    void playerFieldsUpdated(const FlatFieldMap& fields) override { playerHandler_->fieldsUpdated(fields); }
    void playerFieldsArrived(const FlatFieldMap& fields, bool isCreate, FrameXmlEvents& packetEvents) override {
        playerHandler_->playerFieldsArrived(fields, isCreate, packetEvents);
    }
    void shapeshiftFormArrived(uint8_t form, FrameXmlEvents& packetEvents) override {
        playerHandler_->shapeshiftFormArrived(form, packetEvents);
    }
    void playerUnitFlagsArrived(uint32_t previous, uint32_t flags, FrameXmlEvents& packetEvents) override {
        combatHandler_->playerUnitFlagsArrived(previous, flags, packetEvents);
        playerHandler_->unitFlagsArrived(previous, flags);
    }
    void playerVisibilityFlagsArrived(uint8_t previous, uint8_t flags) override {
        playerHandler_->visibilityFlagsArrived(previous, flags);
    }
    void playerDynamicFlagsArrived(uint32_t previous, uint32_t flags, FrameXmlEvents& packetEvents) override {
        combatHandler_->playerDynamicFlagsArrived(previous, flags, packetEvents);
    }
    void playerObjectCreated(std::optional<uint32_t> health, uint32_t dynamicFlags,
                             std::optional<uint32_t> playerFlags) override {
        combatHandler_->playerCreated(health, dynamicFlags, playerFlags);
    }
    void unitHealthArrived(uint64_t guid, uint32_t previous, uint32_t health,
                           FrameXmlEvents& packetEvents) override {
        combatHandler_->unitHealthArrived(guid, previous, health, packetEvents);
    }
    void ownCorpseSeen(uint64_t guid, bool hasPosition, float serverX, float serverY, float serverZ) override {
        combatHandler_->ownCorpseSeen(guid, hasPosition, serverX, serverY, serverZ);
    }
    void playerAuraFieldsArrived(const FlatFieldMap& fields, FrameXmlEvents& packetEvents) override {
        spellHandler_->playerAuraFieldsArrived(fields, packetEvents);
    }
    void skillFieldsArrived(const FlatFieldMap& fields) override { spellHandler_->extractSkillFields(fields); }
    void exploredZoneFieldsArrived(const FlatFieldMap& fields) override {
        spellHandler_->extractExploredZoneFields(fields);
    }
    void companionFieldsArrived(const FlatFieldMap& blockFields) override {
        spellHandler_->companionFieldsArrived(blockFields);
    }
    void questFieldsArrived(const FlatFieldMap& fields) override { questHandler_->applyQuestStateFromFields(fields); }
    uint64_t autoAttackTargetGuid() const override { return combatHandler_->getAutoAttackTargetGuid(); }
    void targetGone() override { combatHandler_->targetGone(); }
    void invalidateTabCycle() override { combatHandler_->invalidateTabCycle(); }
    void removeHostileAttacker(uint64_t guid) override { combatHandler_->removeHostileAttacker(guid); }
    void removeCombatTextForGuid(uint64_t guid) override { combatHandler_->removeCombatTextForGuid(guid); }
    void forgetUnitAuras(uint64_t guid) override { spellHandler_->removeUnitAuraCache(guid); }
    void forgetUnitCast(uint64_t guid) override { spellHandler_->removeUnitCastState(guid); }
    void forgetQuestGiver(uint64_t guid) override { questHandler_->forgetQuestGiver(guid); }
    const std::unordered_map<uint64_t, std::string>& playerNameCache() const override {
        return queryCacheHandler_->getPlayerNameCache();
    }
    void requestCreatureInfo(uint32_t entry, uint64_t guid) override {
        queryCacheHandler_->queryCreatureInfo(entry, guid);
    }
    void playerLeftView(uint64_t guid) override { queryCacheHandler_->playerLeftView(guid); }
    bool holdsItemObject(uint64_t guid) const override { return inventoryHandler_->holdsItemObject(guid); }
    bool trackItemObject(uint64_t guid, bool isContainer, const FlatFieldMap& fields) override {
        return inventoryHandler_->trackItemObject(guid, isContainer, fields);
    }
    void itemObjectChanged(uint64_t guid, bool isContainer, const FlatFieldMap& fields,
                           FrameXmlEvents& packetEvents) override {
        inventoryHandler_->itemObjectChanged(guid, isContainer, fields, packetEvents);
    }
    bool forgetItemObject(uint64_t guid) override { return inventoryHandler_->forgetItemObject(guid); }
    void inventoryFieldsArrived(const FlatFieldMap& fields, bool isCreate, FrameXmlEvents& packetEvents) override {
        inventoryHandler_->inventoryFieldsArrived(fields, isCreate, packetEvents);
    }
    void detectInventorySlotBases(const FlatFieldMap& fields) override {
        inventoryHandler_->detectInventorySlotBases(fields);
    }
    void detectCoinageField(const FlatFieldMap& before, const FlatFieldMap& after) override {
        inventoryHandler_->detectCoinageField(before, after);
    }
    uint32_t expectedMoneyDelta() const override { return inventoryHandler_->expectedMoneyDelta(); }
    void maybeDetectVisibleItemLayout() override { inventoryHandler_->maybeDetectVisibleItemLayout(); }
    void rebuildOnlineInventory() override { inventoryHandler_->rebuildOnlineInventory(); }
    void updateBatchFinished(bool newItemCreated, const FlatFieldMap& playerFields) override {
        inventoryHandler_->updateBatchFinished(newItemCreated, playerFields);
    }
    void updateOtherPlayerVisibleItems(uint64_t guid, const FlatFieldMap& fields) override {
        inventoryHandler_->updateOtherPlayerVisibleItems(guid, fields);
    }
    void forgetPlayerEquipment(uint64_t guid) override { inventoryHandler_->forgetPlayerEquipment(guid); }
    void emitAllOtherPlayerEquipment() override { inventoryHandler_->emitAllOtherPlayerEquipment(); }
    bool loggingOut() const override;

    // ReputationQueries (refreshUnitHostility is declared with the rest of the
    // faction table below).
    reputation::IReputationFeed* reputationFeed() override;
    uint8_t playerRace() const override { return getPlayerRace(); }
    uint8_t playerClass() const override { return getPlayerClass(); }

    /**
     * Get current connection state
     */
    WorldState getState() const override { return state; }

    /// Packets received and not yet handled. update() handles them within a
    /// time budget per frame, so a burst can take several frames.
    size_t queuedIncomingPacketCount() const { return pendingIncomingPackets_.size(); }

    /**
     * Request character list from server
     * Must be called when state is READY or AUTHENTICATED
     */
    void requestCharacterList();

    /**
     * Get list of characters (available after CHAR_LIST_RECEIVED state)
     */
    const std::vector<Character>& getCharacters() const { return characters; }

    void createCharacter(const CharCreateData& data);
    void deleteCharacter(uint64_t characterGuid);


    /**
     * Select and log in with a character
     * @param characterGuid GUID of character to log in with
     */
    void selectCharacter(uint64_t characterGuid);
    void setActiveCharacterGuid(uint64_t guid) { activeCharacterGuid_ = guid; }
    uint64_t getActiveCharacterGuid() const { return activeCharacterGuid_; }
    const Character* getActiveCharacter() const;

    /**
     * Get current player movement info
     */
    const MovementInfo& getMovementInfo() const { return movementInfo; }
    uint32_t getCurrentMapId() const { return currentMapId_; }
    bool getHomeBind(uint32_t& mapId, glm::vec3& pos) const {
        if (!hasHomeBind_) return false;
        mapId = homeBindMapId_;
        pos = homeBindPos_;
        return true;
    }
    uint32_t getHomeBindZoneId() const { return homeBindZoneId_; }

    /**
     * Send a movement packet
     * @param opcode Movement opcode (MSG_MOVE_START_FORWARD, etc.)
     */
    void sendMovement(Opcode opcode) override;

    /**
     * Update player position
     * @param x X coordinate
     * @param y Y coordinate
     * @param z Z coordinate
     */

    /**
     * Update player orientation
     * @param orientation Facing direction in radians
     */

    /**
     * Get entity manager (for accessing entities in view)
     */
    /// Whoever this client is dealing with right now - the unit behind an open
    /// window. The interface calls it "npc" and "questnpc" and puts its face in
    /// the panel's portrait: the gossip frame, the quest frame, the merchant,
    /// the flight master, the trainer, the trade partner.
    ///
    /// Answered from the windows that are open rather than from a guid tracked
    /// on the side. Every one of these already knows who it is talking to, and
    /// a separate record of the same fact is one more thing to leave stale.
    /// The order is the order a window would cover another: a trade or a
    /// merchant is more specific than the gossip that opened it.
    uint64_t getInteractNpcGuid() const;
    /// The innkeeper whose bind confirmation is open, and the trainer whose
    /// talent-wipe confirmation is. Both dialogs close themselves when the
    /// player walks out of range, which is the only thing these answer.
    uint64_t getBinderGuid() const { return binderGuid_; }

    EntityManager& getEntityManager() { return worldHandler_->getEntityManager(); }
    const EntityManager& getEntityManager() const { return worldHandler_->getEntityManager(); }

    /**
     * Send a chat message
     * @param type Chat type (SAY, YELL, WHISPER, etc.)
     * @param message Message text
     * @param target Target name (for whispers, empty otherwise)
     */
    /// Tell the server whether to pass on every loot roll for us.
    void sendOptOutOfLoot(bool optOut);
    /// Announce a rune's type and readiness to the interface.
    void fireRuneUpdate(uint32_t index);
    /// The members of a chat channel, as of the last list requested for it.
    const std::vector<ChannelMember>& getChannelRoster(const std::string& channel) const;

    // Chat auto-join settings (aliased from handler_types.hpp)
    using ChatAutoJoin = game::ChatAutoJoin;
    ChatAutoJoin chatAutoJoin;
    void autoJoinDefaultChannels();

    // Addon chat event callback: fires when any chat message is received (for Lua event dispatch)

    // Generic addon event callback: fires named events with string args
    using AddonEventCallback = std::function<void(const std::string&, const std::vector<std::string>&)>;
    void setAddonEventCallback(AddonEventCallback cb) { addonEventCallback_ = std::move(cb); }

    /// Run a line of the interface's own Lua.
    ///
    /// The keys this client binds are its own, and the windows they open are
    /// its own too - so with an element handed over the key toggled a window
    /// that is no longer drawn and FrameXML never heard about it. Pressing C
    /// with the character sheet handed over did nothing at all for that
    /// reason. This is how a key reaches the frame that replaced it.
    using InterfaceCommand = std::function<void(const std::string&)>;
    void setInterfaceCommandCallback(InterfaceCommand cb) {
        interfaceCommand_ = std::move(cb);
    }
    void runInterfaceCommand(const std::string& lua) override;

    // Spell icon path resolver: spellId -> texture path string (e.g., "Interface\\Icons\\Spell_Fire_Fireball01")
    using SpellIconPathResolver = std::function<std::string(uint32_t)>;
    void setSpellIconPathResolver(SpellIconPathResolver r) { spellIconPathResolver_ = std::move(r); }
    std::string getSpellIconPath(uint32_t spellId) const {
        return spellIconPathResolver_ ? spellIconPathResolver_(spellId) : std::string{};
    }

    // The same SpellIcon.dbc, reached by icon id rather than through a spell.
    // SkillLine.dbc names an icon directly, so a spellbook tab has one without
    // any spell standing between it and the picture.
    using IconPathResolver = std::function<std::string(uint32_t)>;
    void setIconPathResolver(IconPathResolver r) { iconPathResolver_ = std::move(r); }
    std::string getIconPath(uint32_t iconId) const {
        return iconPathResolver_ ? iconPathResolver_(iconId) : std::string{};
    }

    // Spell data resolver: spellId -> {castTimeMs, minRange, maxRange}
    using SpellDataInfo = game::SpellDataInfo;
    using SpellDataResolver = std::function<SpellDataInfo(uint32_t)>;
    void setSpellDataResolver(SpellDataResolver r) { spellDataResolver_ = std::move(r); }
    SpellDataInfo getSpellData(uint32_t spellId) const {
        return spellDataResolver_ ? spellDataResolver_(spellId) : SpellDataInfo{};
    }

    // Item icon path resolver: displayInfoId -> texture path (e.g., "Interface\\Icons\\INV_Sword_04")
    using ItemIconPathResolver = std::function<std::string(uint32_t)>;
    void setItemIconPathResolver(ItemIconPathResolver r) { itemIconPathResolver_ = std::move(r); }
    std::string getItemIconPath(uint32_t displayInfoId) const {
        return itemIconPathResolver_ ? itemIconPathResolver_(displayInfoId) : std::string{};
    }

    // Random property/suffix name resolver: randomPropertyId -> suffix name (e.g., "of the Eagle")
    // Positive IDs → ItemRandomProperties.dbc; negative IDs → ItemRandomSuffix.dbc (abs value)
    using RandomPropertyNameResolver = std::function<std::string(int32_t)>;
    void setRandomPropertyNameResolver(RandomPropertyNameResolver r) { randomPropertyNameResolver_ = std::move(r); }
    std::string getRandomPropertyName(int32_t id) const {
        return randomPropertyNameResolver_ ? randomPropertyNameResolver_(id) : std::string{};
    }

    // Random-suffix/property stat bonuses. Given the item's signed randomPropertyId and its
    // suffixFactor (ITEM_FIELD_PROPERTY_SEED), returns the rolled {statType, value} pairs a
    // green/blue item gains from "of the Bear" etc. statType uses the ITEM_MOD codes shared
    // with ItemDef::ExtraStat (4=Str, 7=Sta, 45=SpellPower, ...). Suffix stats scale as
    // AllocationPct*suffixFactor/10000; positive properties use the enchant's fixed amount.
    using RandomStatBonus = game::RandomStatBonus;
    using RandomStatResolver = std::function<std::vector<RandomStatBonus>(int32_t, uint32_t)>;
    void setRandomStatResolver(RandomStatResolver r) { randomStatResolver_ = std::move(r); }
    std::vector<RandomStatBonus> getRandomStatBonuses(int32_t id, uint32_t suffixFactor) const {
        return randomStatResolver_ ? randomStatResolver_(id, suffixFactor)
                                   : std::vector<RandomStatBonus>{};
    }

    /**
     * Get chat history (recent messages)
     * @param maxMessages Maximum number of messages to return (0 = all)
     * @return Vector of chat messages
     */

    /**
     * Add a locally-generated chat message (e.g., emote feedback)
     */
    void addLocalChatMessage(const MessageChatData& msg) override;
    contacts::IContactsServerFeed* contactsFeed() override;
    who::IWhoFeed* whoFeed() override;
    duel::IDuelsFeed* duelFeed() override;
    petition::IPetitionsFeed* petitionFeed() override;
    arena::IArenaTeamsFeed* arenaFeed() override;
    battleground::IBattlegroundsFeed* battlegroundFeed() override;
    lfg::ILfgFeed* lfgFeed() override;
    calendar::ICalendarFeed* calendarFeed() override;
    group::IGroupServerFeed* groupFeed() override;
    const group::IGroup* group() const override;
    guild::IGuildServerFeed* guildFeed() override;
    auction::IAuctionHouseFeed* auctionFeed() override;
    mail::IMailboxFeed* mailFeed() override;
    guild_bank::IGuildBankFeed* guildBankFeed() override;
    trade::ITradeFeed* tradeFeed() override;
    loot::ILootFeed* lootFeed() override;
    const loot::ILoot* loot() const override;
    const guild_bank::IGuildBank* guildBank() const override;

    // Inventory
    void markOnlineEquipmentDirty() override { inventoryHandler_->markOnlineEquipmentDirty(); }

    using InspectArenaTeam = game::InspectArenaTeam;

    std::string getWhoAreaName(uint32_t zoneId) const { return getAreaName(zoneId); }

    // Network latency (milliseconds, updated each PONG response)
    uint32_t getLatencyMs() const { return lastLatency; }

    /// Turn the character to face a canonical yaw, and tell the server.
    ///
    /// Setting movementInfo.orientation and sending MSG_MOVE_SET_FACING is not
    /// enough on its own: the frame loop resyncs orientation from the renderer's
    /// character facing every frame and re-sends it once it differs by more than
    /// three degrees, so a facing that exists only in the packet is undone
    /// before anything with a cast time completes. Turning the character makes
    /// the two agree, and keeps them agreeing.
    void faceCanonicalYaw(float canonicalYaw);

    // Stand state
    void setStandState(uint8_t state) override;  // 0=stand, 1=sit, 2=sit_chair, 3=sleep, 4=sit_low_chair, 5=sit_medium_chair, 6=sit_high_chair, 7=dead, 8=kneel, 9=submerged

    /// Take the glyph out of a socket. Zero-based slot, as the server counts.
    void removeGlyphFromSocket(uint32_t slot);
    /// Tell the server the GM's reply has been read and closed. Empty packet;
    /// the server answers by deciding whether to offer a survey.
    void resolveGMResponse();
    // Returns the guildId for a player entity (from PLAYER_GUILDID update field).
    uint32_t getEntityGuildId(uint64_t guid) const;

    // AFK/DND status
    bool isAfk() const;
    bool isDnd() const;

    /// A creature template entry turned into something that can be drawn, by
    /// asking the server if it has not already.
    ///
    /// Model:SetCreature is handed an *entry* - a summon spell's EffectMiscValue
    /// for a companion, which is what GetCompanionInfo answers with - and every
    /// model path in this client is found by CreatureDisplayInfo id. The two are
    /// separate number spaces, and treating one as the other looks up a real row
    /// belonging to something else: a wrong model, wearing the skins of a third
    /// creature or none at all. Only the server knows the mapping, so this
    /// answers zero the first time and the query fills the cache for the next
    /// frame to find.
    uint32_t getCreatureDisplayIdForEntry(uint32_t entry);
    void startAutoAttack(uint64_t targetGuid) override;
    void stopAutoAttack() override;
    // Threat
    using ThreatEntry = CombatHandler::ThreatEntry;
    const std::vector<ThreatEntry>* getTargetThreatList() const;

    void cancelCast() override;

    /// Forget a primary profession. The server refuses anything that is not
    /// one, so the skill panel's unlearn button is the only sensible caller.
    void unlearnSkill(uint32_t skillId);
    /// Reset every instance this player, or their group, is saved to. The
    /// server does the whole set at once; there is nothing to name.
    void resetInstances();

    // Player proficiency bitmasks (from SMSG_SET_PROFICIENCY)
    // itemClass 2 = Weapon (subClassMask bits: 0=Axe1H,1=Axe2H,2=Bow,3=Gun,4=Mace1H,5=Mace2H,6=Polearm,7=Sword1H,8=Sword2H,10=Staff,13=Fist,14=Misc,15=Dagger,16=Thrown,17=Crossbow,18=Wand,19=Fishing)
    // itemClass 4 = Armor (subClassMask bits: 1=Cloth,2=Leather,3=Mail,4=Plate,6=Shield)
    uint32_t getWeaponProficiency() const { return weaponProficiency_; }
    uint32_t getArmorProficiency()  const { return armorProficiency_; }
    bool canUseWeaponSubclass(uint32_t subClass) const { return (weaponProficiency_ >> subClass) & 1u; }
    bool canUseArmorSubclass(uint32_t subClass)  const { return (armorProficiency_  >> subClass) & 1u; }

    // Minimap pings from party members
    using MinimapPing = game::MinimapPing;

    // Glyphs (WotLK): up to 6 glyph slots per spec (3 major + 3 minor)
    static constexpr uint8_t MAX_GLYPH_SLOTS = 6;

    void saveCharacterConfig();
    void loadCharacterConfig();
    static std::string getCharacterConfigDir();

    // Auras - delegate to SpellHandler as canonical authority
    /// Whether the player is currently carrying this spell's aura.
    ///
    /// The reliable way to ask "am I already riding the mount this button
    /// summons": the mount you are on put its own aura on you, and no other
    /// mount's aura is there. Movement's mountAuraSpellId is a guess by comparison - it
    /// is whatever the client could work out at the moment the mount appeared,
    /// and when the player did not cast it themselves (a login already mounted,
    /// a taxi, a server-applied aura) the blind scan behind it can land on a
    /// racial or a tracking buff instead.
    [[nodiscard]] bool playerCarriesAura(uint32_t spellId) const {
        if (spellId == 0) return false;
        for (const auto& aura : spellHandler_->getPlayerAuras()) {
            if (!aura.isEmpty() && aura.spellId == spellId) return true;
        }
        return false;
    }

    // Snap the character to face the camera's current look direction and return the
    // resulting canonical orientation. Used by fishing so the bobber lands in front of
    // where the player is looking: while standing still the character yaw does not track
    // the free-look camera, so without this the bobber would drop toward a stale heading.
    using FaceCameraProvider = std::function<float()>;
    void setFaceCameraProvider(FaceCameraProvider cb) { faceCameraProvider_ = std::move(cb); }
    float faceCameraDirection() {
        return faceCameraProvider_ ? faceCameraProvider_() : getMovementInfo().orientation;
    }

    // Global Cooldown (GCD) - set when the server sends a spellId=0 cooldown entry
    bool isGCDActive() const { return spellHandler_->getGCDRemaining() > 0.0f; }

    // Player skills
    bool isSkillCategoryCollapsed(uint32_t categoryId) const {
        return collapsedSkillCategories_.count(categoryId) > 0;
    }
    void setSkillCategoryCollapsed(uint32_t categoryId, bool collapsed);

    void unstuck();
    void unstuckGy();
    void unstuckHearth();

    // Faction hostility map (populated from FactionTemplate.dbc by Application)
    void setFactionHostileMap(std::unordered_map<uint32_t, bool> map) { factionHostileMap_ = std::move(map); }
    void setFactionFriendlyMap(std::unordered_map<uint32_t, bool> map) { factionFriendlyMap_ = std::move(map); }
    /// The templates whose faction the player has a standing with, by that
    /// standing's index in the server's list. See factionAtWarLive.
    void setFactionTemplateRepList(std::unordered_map<uint32_t, uint32_t> map) {
        factionTemplateRepList_ = std::move(map);
    }
    /// Whether a beneficial spell may be aimed at this faction.
    ///
    /// Not the negation of isHostileFaction: a faction can be neither, and most
    /// wildlife is. Unknown answers false, so a spell falls back to the caster
    /// rather than being sent at something the server will refuse.
    bool isFriendlyFaction(uint32_t factionTemplateId) const {
        if (const int atWar = factionAtWarLive(factionTemplateId); atWar >= 0) return atWar == 0;
        auto it = factionFriendlyMap_.find(factionTemplateId);
        return it != factionFriendlyMap_.end() ? it->second : false;
    }
    /// Re-judge every unit's hostility, after the at-war state it came from
    /// has changed. A unit's is worked out when it appears and kept.
    void refreshUnitHostility() override;
    /// How a unit regards the player, numbered as UnitReaction numbers it:
    /// 1 hated to 8 exalted. For a faction the player has a standing with,
    /// that standing - no better than neutral while at war - and otherwise 2
    /// hostile, 5 friendly, 4 neither.
    ///
    /// Not the same question as isHostileFaction, which is whether the player
    /// may attack it. The Kurenai are not attackable by an Alliance player at
    /// any standing, but at Unfriendly they will not talk to one either, and
    /// the server refuses the conversation on exactly this number.
    [[nodiscard]] int unitReactionToPlayer(const Unit& unit) const;

    // Notify that a transport has been spawned (called after WMO instance creation)
    void notifyTransportSpawned(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation);

    TransportManager* getTransportManager() { return transportManager_.get(); }

    // Player GUID
    uint64_t getPlayerGuid() const { return playerGuid; }

    uint8_t getPlayerClass() const {
        const Character* ch = getActiveCharacter();
        return ch ? static_cast<uint8_t>(ch->characterClass) : 0;
    }
    uint8_t getPlayerRace() const {
        const Character* ch = getActiveCharacter();
        return ch ? static_cast<uint8_t>(ch->race) : 0;
    }
    // Horde races: Orc, Undead, Tauren, Troll, Goblin, Blood Elf. Everything
    // else (including an unknown/zero race) defaults to Alliance.
    bool isPlayerAlliance() const {
        switch (static_cast<Race>(getPlayerRace())) {
            case Race::ORC: case Race::UNDEAD: case Race::TAUREN:
            case Race::TROLL: case Race::GOBLIN: case Race::BLOOD_ELF:
                return false;
            default:
                return true;
        }
    }
    void setPlayerGuid(uint64_t guid) { playerGuid = guid; }

    // Barber shop
    bool isBarberShopOpen() const { return barberShopOpen_; }
    void closeBarberShop() { barberShopOpen_ = false; fireAddonEvent("BARBER_SHOP_CLOSE", {}); }

    /// The bind-or-leave prompt raised on entering an instance in progress.
    /// False when nothing is pending, in which case the out-params are
    /// untouched.
    bool getInstanceLockPrompt(float& secondsLeft, bool& previouslySaved,
                               uint32_t& completedMask) const;
    bool isInInstance() const;

    // Boss encounter unit tracking (SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT)
    static constexpr uint32_t kMaxEncounterSlots = 5;

    std::string getMapName(uint32_t mapId) const;

    /// How many boss encounters a map has, for the "%d of %d bosses" line.
    /// Counted from DungeonEncounter.dbc, which is the only place the total
    /// lives - the server sends which are done and never how many there are.
    uint32_t getDungeonEncounterCount(uint32_t mapId, uint32_t difficulty) const;

    // ---- Loot ----
    /// Asked by the spell handler once a gather or an opening finishes, and by
    /// the deferred open of a used game object.
    void lootTarget(uint64_t guid) override;
    void scheduleGameObjectLootOpen(uint64_t guid, float delaySeconds = 0.35f, uint8_t attempts = 1);

    /// True once the object is known to be a fishing school. A click on one
    /// before its query response arrives still schedules a loot open, and by the
    /// time that fires the metadata has usually landed - so the deferred open
    /// re-checks rather than harvesting a school the player never fished.
    bool isFishingHoleGameObject(uint64_t guid) const;
    void clearPendingGameObjectLootOpen(uint64_t guid);
    bool hasPendingGameObjectLootOpen(uint64_t guid) const;
    bool isGatherGameObject(uint64_t guid) const;
    void despawnGameObjectLocally(uint64_t guid) override;
    /// Remove a creature corpse client-side once it has been looted empty.
    void despawnCreatureLocally(uint64_t guid) override;
    /// Turn the player to face the target when an attack or a targeted spell
    /// starts. Off by default, as the original client has it: a target behind
    /// the player has to be faced first, and the server says so. A debugging
    /// aid, set from the Combat page.
    void setAutoFaceTarget(bool enabled) { autoFaceTarget_ = enabled; }
    [[nodiscard]] bool isAutoFaceTarget() const { return autoFaceTarget_; }

    /// Whether a friendly spell cast with nothing friendly selected falls back
    /// to the caster. On by default, as the real client is, and turned off
    /// through the interface's autoSelfCast option - which offered the choice
    /// while the behaviour was unconditional.
    void setAutoSelfCast(bool enabled) { autoSelfCast_ = enabled; }
    bool isAutoSelfCast() const { return autoSelfCast_; }

    // Equipment Sets (aliased from handler_types.hpp)
    using EquipmentSetInfo = game::EquipmentSetInfo;

    // NPC Gossip
    void interactWithNpc(uint64_t guid);
    void interactWithGameObject(uint64_t guid);
    uint64_t getHookedFishingBobberGuid() const { return hookedFishingBobberGuid_; }
    void closeGossip() override;

    // Quest log
    using QuestLogEntry = QuestHandler::QuestLogEntry;
    /// The quests whose objective areas the world map shades.
    ///
    /// Set by WorldMapBlobFrame:DrawQuestBlob, which FrameXML calls with false
    /// for every quest as it rebuilds the map's list and with true for the one
    /// the player selected. View state, so it is not saved: the map decides it
    /// again the moment it opens.
    bool isQuestBlobShown(uint32_t questId) const {
        return blobQuestIds_.count(questId) > 0;
    }
    void setQuestBlobShown(uint32_t questId, bool shown) {
        if (shown) blobQuestIds_.insert(questId);
        else       blobQuestIds_.erase(questId);
    }
    std::optional<uint32_t> getWorldState(uint32_t key) const {
        auto it = worldStates_.find(key);
        if (it == worldStates_.end()) return std::nullopt;
        return it->second;
    }
    uint32_t getWorldStateMapId() const { return worldStateMapId_; }
    uint32_t getWorldStateZoneId() const { return worldStateZoneId_; }

    // Mirror timers (0=fatigue, 1=breath, 2=feigndeath)
    //
    // The interface names its timers rather than numbering them, and the name
    // is a table key twice over: MirrorTimerColors is keyed by it, and
    // MirrorTimer_Show reads color.r straight off the result, so a name that
    // misses raises rather than merely losing the colour. MirrorTimer_Hide then
    // matches the running dialog by the same name, which is why start and stop
    // have to agree - they are one table here for that reason. Four copies of
    // this list had accumulated across three files and the fourth said
    // "FATIGUE", which is not one of the four keys mirrortimer.lua defines.
    static constexpr const char* kMirrorTimerNames[3] =
        {"EXHAUSTION", "BREATH", "FEIGNDEATH"};
    static constexpr const char* kMirrorTimerLabels[3] =
        {"Exhaustion", "Breath", "Feign Death"};

    struct MirrorTimer {
        int32_t value    = 0;
        int32_t maxValue = 0;
        int32_t scale    = 0;     // +1 = counting up, -1 = counting down
        bool    paused   = false;
        bool    active   = false;
        float   pendingMs = 0.0f; // sub-millisecond carry for the local countdown
    };
    const MirrorTimer& getMirrorTimer(int type) const {
        static MirrorTimer empty;
        return (type >= 0 && type < 3) ? mirrorTimers_[type] : empty;
    }
    /**
     * Count the mirror timers (breath, fatigue, feign death) down locally.
     *
     * The server sends SMSG_START_MIRROR_TIMER once with the remaining time and a
     * scale, then only speaks again when something changes - so without this the
     * breath bar just sits at whatever value it was handed and never moves.
     * scale is ms of timer per ms of real time: -1 while drowning, +1 while the
     * bar refills at the surface.
     */
    void tickMirrorTimers(float dt) {
        if (dt <= 0.0f) return;
        const float elapsedMs = dt * 1000.0f;
        for (size_t ti = 0; ti < 3; ++ti) {
            auto& t = mirrorTimers_[ti];
            if (!t.active || t.paused || t.scale == 0 || t.maxValue <= 0) continue;
            // Carry the sub-millisecond remainder: truncating each frame would lose
            // most of a 144fps frame's 6.94ms and run the timer visibly slow.
            t.pendingMs += elapsedMs;
            const int32_t wholeMs = static_cast<int32_t>(t.pendingMs);
            if (wholeMs == 0) continue;
            t.pendingMs -= static_cast<float>(wholeMs);
            t.value = std::clamp(t.value + t.scale * wholeMs, 0, t.maxValue);
            // Refilled, so it is over. The server does not say so at this
            // point: on surfacing it sends one update with a positive scale
            // and then nothing until its own counter reaches full, which is
            // several seconds later. Without this the bar climbs to full and
            // sits there at a hundred percent in the meantime, which is what
            // "the breath meter will not go away" looks like.
            //
            // Only when refilling. A drowning bar reaching zero is not over -
            // that is when the damage starts, and the server keeps it up.
            if (t.scale > 0 && t.value >= t.maxValue) {
                t.active = false;
                t.pendingMs = 0.0f;
                // And say so. Clearing the flag only put away the bar this
                // client draws, and the player frame is handed over - which
                // means MirrorTimer1..3 are not suppressed and the bar actually
                // on screen is FrameXML's. It hides on MIRROR_TIMER_STOP and
                // nothing else, so without this it sat there full for good.
                //
                // Named rather than numbered, as the packet handler does:
                // MirrorTimer_Hide matches on the timer's name.
                fireAddonEvent("MIRROR_TIMER_STOP",
                               {ti < 3 ? kMirrorTimerNames[ti] : "BREATH"});
            }
        }
    }

    /// Which extra action bar the current form or stance uses, or 0 for none.
    ///
    /// SpellShapeshiftForm.dbc carries it per form - cat 1, bear 3, moonkin 4,
    /// the three warrior stances 1 to 3, and 0 for the travel forms, which have
    /// no bar of their own. Read from the file rather than written out here,
    /// because a table of class-and-form guesses is not checkable.
    uint32_t getBonusActionBarOffset() const;

    // Death Knight rune state (6 runes: 0-1=Blood, 2-3=Unholy, 4-5=Frost; may become Death=3)
    enum class RuneType : uint8_t { Blood = 0, Unholy = 1, Frost = 2, Death = 3 };
    struct RuneSlot {
        RuneType type = RuneType::Blood;
        bool     ready = true;          // Server-confirmed ready state
        float    readyFraction = 1.0f;  // 0.0=depleted → 1.0=full (from server sync)
    };
    const std::array<RuneSlot, 6>& getPlayerRunes() const { return playerRunes_; }

    // Talent-driven spell modifiers (SMSG_SET_FLAT_SPELL_MODIFIER /
    // SMSG_SET_PCT_SPELL_MODIFIER), kept by the spell book.
    using SpellModOp = game::SpellModOp;

    // Convenience: apply flat+pct modifier to a base value.
    // result = (base + flatMod) * (1.0 + pctMod/100.0), clamped to >= 0.
    static int32_t applySpellMod(int32_t base, int32_t flat, int32_t pct) {
        int64_t v = static_cast<int64_t>(base) + flat;
        if (pct != 0) v = v + (v * pct + 50) / 100;  // round half-up
        return static_cast<int32_t>(v < 0 ? 0 : v);
    }

    // Shaman totems (4 slots: 0=Earth, 1=Fire, 2=Water, 3=Air), kept by the
    // spell book.
    static constexpr int NUM_TOTEM_SLOTS = kTotemSlots;
    /// Pull down a totem by the slot it stands in.
    void destroyTotem(int slot);

    // ---- Readable text (books / scrolls / notes) ----
    /// PageTextMaterial.dbc's name for a material id, empty for none. The
    /// interface wants the word, not the number - it picks a texture and a
    /// text colour by it.
    const std::string& getPageTextMaterialName(uint32_t materialId) const;
    /// Languages.dbc's name for a language id - "Common", "Orcish", and the
    /// fourteen others. Zero is Universal, which the file does not carry
    /// because it is not a language anyone speaks.
    ///
    /// The interface wants the word: ChatFrame_MessageEventHandler compares
    /// the third chat argument against GetDefaultLanguage() and prefixes
    /// "[<it>] " when they differ, so a number there is printed verbatim.
    const std::string& getLanguageName(uint32_t languageId) const;


    /// What a vendor wants besides coin: honor, arena points, or up to five
    /// items of a given count. ItemExtendedCost.dbc, keyed by the id a vendor
    /// item carries.
    struct ExtendedCostEntry {
        uint32_t honorPoints = 0;
        uint32_t arenaPoints = 0;
        uint32_t itemId[5] = {};
        uint32_t itemCount[5] = {};
    };
    /// Null when the id is unknown or the DBC is not there.
    ///
    /// Lives here rather than in the window that draws vendors, because the
    /// original interface asks for the same thing through
    /// GetMerchantItemCostItem and cannot reach into this client's UI.
    const ExtendedCostEntry* getExtendedCost(uint32_t extendedCostId) const;

    /// The rectangle a whole continent's map covers, in server coordinates.
    ///
    /// WorldMapArea.dbc's row for the continent itself - the one whose AreaID is
    /// zero - which is what anything placing a marker on a continent-wide map
    /// projects against. The world map has read this for a long time, inside
    /// `rendering/world_map`; the flight map needs the same rectangle and
    /// cannot reach in there, so it is read here on first ask like every other
    /// DBC-backed cache.
    struct ContinentBounds {
        float left = 0, right = 0, top = 0, bottom = 0;
        bool valid = false;
    };
    const ContinentBounds& getContinentBounds(uint32_t mapId) const;

    // ---- Battlegrounds (BattlemasterList.dbc) ----
    // Thirteen rows: ID=0, MapID=1, Name=11, MaxGroupSize=28, MinLevel=30,
    // MaxLevel=31. Verified against the file rather than inferred - the name
    // sits past sixteen locale slots, so a field either side of it reads as a
    // fragment of the previous string.
    struct BattlemasterEntry {
        uint32_t    id = 0;
        std::string name;
        uint32_t    maxGroupSize = 0;
        uint32_t    minLevel = 0;
        uint32_t    maxLevel = 0;
        /// 3 = battleground, 4 = arena. The two share this table and the
        /// battleground list must not offer arenas.
        uint32_t    instanceType = 0;
        /// How many maps the row names. Every real battleground names one; the
        /// two rows that stand for a pool of them - Random Battleground, All
        /// Arenas - name several, which is how a random entry is told apart
        /// without hardcoding its id.
        uint32_t    mapCount = 0;
        /// The maps themselves, which is what says whether the world the player
        /// just entered is a battleground.
        std::vector<uint32_t> mapIds;
    };
    const BattlemasterEntry* getBattlemasterInfo(uint32_t bgTypeId);

    /// Every battleground in BattlemasterList.dbc, arenas excluded, ordered by
    /// id so an index into it means the same thing from one call to the next.
    const std::vector<BattlemasterEntry>& getBattlegroundTypes();

    /// Whether a map id belongs to a battleground, from the same table.
    bool isBattlegroundMap(uint32_t mapId);
    /// Whether a map id belongs to an arena. Same table, the other instance
    /// type - the rows are loaded either way and only the battleground ones
    /// are kept in the queue list.
    bool isArenaMap(uint32_t mapId);

    // ---- Currencies (CurrencyTypes.dbc) ----
    // In 3.3.5a a currency is a row pointing at an item, and the amount held is
    // that item's stack count in the bags - there is no separate store to read.
    struct CurrencyType {
        uint32_t id = 0;
        uint32_t itemId = 0;
    };
    const std::vector<CurrencyType>& getCurrencyTypes();

    /// The spell a glyph is, by GlyphProperties.dbc id.
    ///
    /// The server sends glyph *properties* ids in the update fields, and every
    /// caller wants the spell: its name, its icon and its tooltip are the
    /// spell's. Verified rather than assumed - field 1 resolves to a real spell
    /// for 361 of the 362 rows, and those spells are named "Glyph of Moonfire"
    /// and the like.
    void ensureGlyphPropertiesLoaded();
    uint32_t getGlyphSpellId(uint32_t glyphPropertiesId) const {
        auto it = glyphSpellCache_.find(glyphPropertiesId);
        return it != glyphSpellCache_.end() ? it->second : 0u;
    }

    /// A line for the error frame (spell can't be cast, etc.): UI_ERROR_MESSAGE
    /// for the interface, which draws it, and UiErrorRaised for the client,
    /// which sounds it.
    void addUIError(const std::string& msg);
    /// A script that failed, shown on screen without telling any script about it.
    ///
    /// Deliberately not addUIError. That fires UI_ERROR_MESSAGE, which is a Lua
    /// event, and UIErrorsFrame is registered for it - so reporting a script
    /// error ran script, and if that script errored it was reported the same
    /// way. One broken handler fed itself as fast as the frame loop allowed
    /// until the process died, which is not what a Lua error looks like from
    /// outside and cost a long hunt through the renderer.
    ///
    /// UI_ERROR_MESSAGE is also the wrong event for this: it carries the game's
    /// own refusals - out of range, not enough mana - and the real client
    /// reports script errors somewhere else entirely.
    void addScriptError(const std::string& msg);
    void fireAddonEvent(const std::string& event, const std::vector<std::string>& args = {}) {
        if (addonEventCallback_) pendingAddonEvents_.event(event, args);
    }

    /// Interface phase only. Notifications produced by handlers wait for the next drain.
    void dispatchAddonEvents();

    bool isHostileFactionPublic(uint32_t factionTemplateId) const { return isHostileFaction(factionTemplateId); }
    bool isPlayerRooted() const { return movementInfo.isPlayerRooted(); }
    bool isGravityDisabled() const { return movementInfo.isGravityDisabled(); }
    bool isFeatherFalling() const { return movementInfo.isFeatherFalling(); }
    bool isWaterWalking() const { return movementInfo.isWaterWalking(); }
    bool isPlayerFlying() const { return movementInfo.isPlayerFlying(); }
    // The player is *allowed* to fly (CAN_FLY set by a flying mount or .gm fly).
    // Drives flight-mode physics: FLYING is only set once actually airborne, so
    // gating flight on isPlayerFlying() left .gm fly unable to take off.
    bool canFly() const {
        return (movementInfo.flags & static_cast<uint32_t>(MovementFlags::CAN_FLY)) != 0;
    }
    bool isHovering() const { return movementInfo.isHovering(); }
    bool isSwimming() const { return movementInfo.isSwimming(); }
    // Set the character pitch angle (radians) for movement packets (flight / swimming).
    // Positive = nose up, negative = nose down.
    void setMovementPitch(float radians) { movementInfo.pitch = radians; }
    void dismount() override;

    /// Accept the innkeeper's offer to make this the player's home. The prompt
    /// arrives as SMSG_BINDER_CONFIRM and this is the reply.
    void confirmBinder();

    // Taxi / Flight Paths
    using TaxiPathNode = MovementHandler::TaxiPathNode;

    // Vehicle (WotLK)
    bool isInVehicle() const { return vehicleId_ != 0; }
    uint32_t getVehicleId() const { return vehicleId_; }
    void sendRequestVehicleExit();

    // Vendor
    void closeVendor() override;

    // Item-targeted item use: sharpening stones, weightstones and weapon oils enchant
    // another item, so using one arms a targeting cursor instead of casting immediately.
    /// Arm the item-picking cursor for a spell that must be cast at an item.
    void beginSpellItemTargeting(uint32_t spellId, const std::string& spellName) override;

    /// Seconds until the server teleports the player out of an instance they
    /// are not valid for, or 0 when no such timer is running. The server sends
    /// the whole countdown once and then says nothing until it stops, so the
    /// remainder is worked out from when it arrived.
    int getInstanceBootTimeRemaining() const;

    /// Merge partial stacks, then order every bag slot, and send the moves.
    ///
    /// Lives here rather than in the window that used to own it because two
    /// things ask for it now: this client's own bag screen and the interface's
    /// SortBags(). One queue, drained a swap per tick - the server refuses a
    /// burst of them, and a sort is dozens.
    void sortBags();
    /// Send these one per tick rather than all at once.
    ///
    /// The server's flood protection drops a burst, which for a sequence of
    /// dot-commands means most of it silently does not happen. Same reason
    /// sortBags has a queue, and the same drain beside it.
    void queuePacedChat(std::vector<std::string> lines);

    /// Whether a sort is still sending. The button reads it to disable itself.
    /// One item sort at a time, whichever container it is over.
    ///
    /// Bags and the bank both move items with CMSG_SWAP_ITEM and the server
    /// takes them one at a time, so they share a queue rather than racing two.
    bool isSortingItems() const { return !sortSwapQueue_.empty(); }
    /// The bank, main slots and bank bags together. Mirrors sortBags: merge
    /// partial stacks, plan the swaps against the merged layout, show the
    /// result at once locally, and let the queue tell the server over the
    /// following ticks.
    void sortBank(int mainSlotCount);
    /// One bank bag, leaving the rest of the bank alone. No stack merging:
    /// pouring partials together across the bank is what Sort All is for, and
    /// doing it here would move items out of the bag being sorted.
    void sortBankBag(int bagIndex);

    // ---- Item socketing ----
    // Flat accessors rather than the session struct: InventoryHandler is only
    // forward-declared here, and a nested type cannot cross that.
    uint32_t getSocketPendingGemItemId(int index) const;

    // Bank
    // Copper cost of the bank bag slot at the given 0-based index (BankBagSlotPrices.dbc).
    uint32_t getBankBagSlotPrice(int slotIndex) const;

    /// One row of a profession's recipe list (game/craft_recipe.hpp).
    using CraftRecipe = game::CraftRecipe;

    /// Substitute WoW description tokens in `raw` using live spell data: $s/$o/$m/$M base
    /// points and $d durations (incl. cross-spell $<spellId> references), plus $l/$g
    /// plural/gender forms. Unresolvable tokens ($h proc chance, $t period) are stripped
    /// cleanly. `selfSpellId` supplies the default source for index-only tokens like $s1.
    /// The evaluator's window onto this client, for $<name> and ${}.
    SpellDescriptionContext descriptionContext(uint32_t spellId,
                                               const std::string* declarations) const;
    /// The $name=expression declarations a spell's description draws on, or
    /// null for a spell that declares none.
    const std::string* descriptionDeclarations(uint32_t spellId) const {
        const SpellNameEntry* spell = spellHandler_->findSpell(spellId);
        return spell ? spellHandler_->spellDescriptionVariables(spell->descriptionVariableId)
                     : nullptr;
    }
    std::string formatSpellDescription(uint32_t selfSpellId, const std::string& raw) const;

    // Item templates - the inventory's.
    void ensureItemInfo(uint32_t entry) override { inventoryHandler_->ensureItemInfo(entry); }

    /**
     * Set callbacks
     */
    void setOnSuccess(WorldConnectSuccessCallback callback) { onSuccess = std::move(callback); }
    void setOnFailure(WorldConnectFailureCallback callback) { onFailure = std::move(callback); }

    /**
     * Update - call regularly (e.g., each frame)
     *
     * @param deltaTime Time since last update in seconds
     */
    void update(float deltaTime);
    /// Client frame phases may poll protocol work separately from simulation.
    /// update() remains the combined entry for callers such as GameHarness.
    void updateNetwork(float deltaTime);
    void updateGame(float deltaTime);
    void updateNetworking();
    /// A world connection that has gone quiet without the socket noticing - a
    /// cable pulled, a server hung - is lost when a ping asking about the
    /// silence goes unanswered. Counts only time spent listening.
    void watchWorldSilence(float deltaTime);
    /// The world connection is gone while the character is in the world: said
    /// once, as Disconnected, with the connection closed and the state
    /// DISCONNECTED. What the game holds stays until the client acts on it.
    void loseWorldConnection();
    void updateTimers(float deltaTime);
    void updateEntityInterpolation(float deltaTime);
    void updateTaxiAndMountState(float deltaTime);

    /**
     * Reset DBC-backed caches so they reload from new expansion data.
     * Called by Application when the expansion profile changes.
     */
    void resetDbcCaches();

    // ═══════════════════════════════════════════════════════════════════
    //  Domain handler access - public accessors for friend-class elimination
    // ═══════════════════════════════════════════════════════════════════

    // ── Handler & Subsystem Accessors (unique_ptr → raw pointer) ─────
    network::WorldConnection* getSocket() override { return socket.get(); }
    const network::WorldConnection* getSocket() const { return socket.get(); }
    /// What the character holds - bags, equipment, coin, books - and the
    /// windows that move it: loot, vendors, mail, the bank, the auction house.
    InventoryHandler& inventory() { return *inventoryHandler_; }
    const InventoryHandler& inventory() const { return *inventoryHandler_; }
    /// The character's quests, the quest givers' windows and their marks.
    QuestHandler& quests() { return *questHandler_; }
    const QuestHandler& quests() const { return *questHandler_; }
    /// The character's conversation: the chat log, channels, whispers, AFK.
    ChatHandler& chat() { return *chatHandler_; }
    const ChatHandler& chat() const { return *chatHandler_; }
    /// The spell book: spells, casts, auras, talents, skills, and what
    /// Spell.dbc and the skill tables say about them.
    SpellHandler& spells() { return *spellHandler_; }
    const SpellHandler& spells() const { return *spellHandler_; }
    /// Combat: auto-attack, who is attacking whom, threat, targeting,
    /// combat text, death and the spirit healer.
    CombatHandler& combat() { return *combatHandler_; }
    const CombatHandler& combat() const { return *combatHandler_; }
    /// Where the player is and how they move: position, speeds, transports,
    /// following, area triggers and taxi flights.
    MovementHandler& movement() { return *movementHandler_; }
    const MovementHandler& movement() const { return *movementHandler_; }
    /// The objects in view as the server describes them: creatures, players,
    /// game objects, their names, kinds, appearance and transports.
    WorldHandler& world() { return *worldHandler_; }
    QueryCacheHandler& queryCache() { return *queryCacheHandler_; }
    const QueryCacheHandler& queryCache() const { return *queryCacheHandler_; }
    const WorldHandler& world() const { return *worldHandler_; }
    /// The rest of the player's dealings with the world and the server: inspect,
    /// played time, logout, summons, instances and their lockouts, the
    /// battlefield manager, minimap pings, GM tickets and reports.
    SocialHandler& social() { return *socialHandler_; }
    const SocialHandler& social() const { return *socialHandler_; }
    /// Key bindings, macros and settings the server keeps for the account.
    AccountDataHandler& accountData() { return *accountDataHandler_; }
    const AccountDataHandler& accountData() const { return *accountDataHandler_; }
    /// The character's action bar.
    ActionBarHandler& actionBar() { return *actionBarHandler_; }
    const ActionBarHandler& actionBar() const { return *actionBarHandler_; }
    /// What time it is on the realm: the world's hour and the calendar moment.
    RealmTimeHandler& realmTime() { return *realmTimeHandler_; }
    const RealmTimeHandler& realmTime() const { return *realmTimeHandler_; }
    /// The weather where the character stands, as the realm says it.
    WeatherHandler& weather() { return *weatherHandler_; }
    const WeatherHandler& weather() const { return *weatherHandler_; }
    /// The character in the world: experience, level, rest, the sheet.
    PlayerHandler& player() { return *playerHandler_; }
    const PlayerHandler& player() const { return *playerHandler_; }

    // ── Mutable Accessors for Members with Existing Const Getters ────
    MovementInfo& movementInfoRef() { return movementInfo; }

    // ── Core / Session ───────────────────────────────────────────────
    uint32_t getBuild() const { return build; }
    const std::vector<uint8_t>& getSessionKey() const override { return sessionKey; }
    auto& charactersRef() { return characters; }
    /// Show an appearance on the player's own model without telling the server.
    ///
    /// The barber shop's preview: the styles are cycled locally and only the
    /// Accept button sends anything. The character record is what the model is
    /// built from, so this writes there and asks for the rebuild - the same
    /// path a confirmed change takes, which is why the preview and the result
    /// cannot look different.
    /// False when there is no record to write to, which is the one way this can
    /// quietly do nothing.
    bool previewPlayerAppearance(uint32_t appearanceBytes, uint8_t facialFeatures);
    auto& updateFieldTableRef() { return updateFieldTable_; }

    // ── Character & Appearance ───────────────────────────────────────

    // ── AFK / DND ────────────────────────────────────────────────────

    // ── Movement & Transport ─────────────────────────────────────────

    // ── Combat & Player Stats ────────────────────────────────────────

    // ── World, Map & Zones ───────────────────────────────────────────
    auto& currentMapIdRef() { return currentMapId_; }

    // ── Home Bind ────────────────────────────────────────────────────

    // ── Trainer, GM & Misc ───────────────────────────────────────────

    // ── UI & Event Callbacks ─────────────────────────────────────────
    bool hasAddonEventListener() const { return static_cast<bool>(addonEventCallback_); }
    void suppressNextMeleeSwingAnim() override { suppressMeleeSwingAnim_ = true; }
    bool consumeSuppressMeleeSwingAnim() override {
        bool v = suppressMeleeSwingAnim_;
        suppressMeleeSwingAnim_ = false;
        return v;
    }

    // ── Methods moved from private (domain handler use) ──────────────
    void addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId,
                       bool isPlayerSource, uint8_t powerType = 0,
                       uint64_t srcGuid = 0, uint64_t dstGuid = 0) override;
    bool shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId) override;

    /// An error the player should see: "Target is too far away", "You have no
    /// target", and their kind.
    ///
    /// On screen only, through addUIError - UIErrorsFrame is the red text above
    /// the middle of the screen, and addons watch UI_ERROR_MESSAGE to catch
    /// failures. That is where the real client puts these and nowhere else.
    ///
    /// It used to write the chat log as well, because the sites converted to it
    /// had been chat-only. In a fight that scrolls the log past everything that
    /// mattered: "Not ready", "Not enough rage" and "You can't do that right
    /// now" arrive once per rejected keypress.
    void raiseUiError(const std::string& message);
    void sendPing() override;
    std::string guidToUnitId(uint64_t guid) const;
    Unit* getUnitByGuid(uint64_t guid);
    void cacheInspectedPlayerEquipment(uint64_t guid, const std::array<uint32_t, 19>& itemEntries,
                                       const std::array<uint16_t, 19>& enchantIds) override;
    std::string getLfgDungeonName(uint32_t dungeonId) const;
    /// The mounts and critters the player knows, as the pet tab lists them.
    ///
    /// Both are spells in the spellbook; what tells them apart is what the
    /// spell does, read from Spell.dbc. A mount applies aura 78; a critter
    /// summons with SummonProperties 41. Rebuilt when the spellbook changes,
    /// because that is the only thing that can change the answer.
    const std::vector<Companion>& getCompanions(bool mounts) const;
    /// "MOUNT", "CRITTER", or empty for a creature that is neither. Both kinds
    /// hold a creature template entry and both need a display id looked up, so
    /// both wait on a query coming back - and the tab is told which list to
    /// re-read, because COMPANION_UPDATE carries the kind.
    std::string companionKindForCreature(uint32_t entry) const;
    /// Fire COMPANION_UPDATE if a mount or critter has come out or gone away.
    ///
    /// Called from every place the player's auras change, and it has to filter:
    /// a player's auras move constantly in combat and the pet tab cares about
    /// two of them.
    void announceCompanionChange() override;

    /// Every row of LFGDungeons.dbc worth offering, in the order it should be
    /// listed. Built once; the file does not change while the client runs.
    const std::vector<LfgDungeon>& getLfgDungeons() const {
        loadLfgDungeonDbc();
        return lfgDungeons_;
    }

    std::string getAreaName(uint32_t areaId) const;

    /// What GetZonePVPInfo answers for a zone: one of "sanctuary", "arena",
    /// "friendly", "hostile", "contested", or empty for a zone with no PvP
    /// character at all. Second is the controlling faction, for the two that
    /// name one.
    ///
    /// Goes through getAreaName first because that is what fills the cache
    /// this reads, and it is lazy.
    std::pair<std::string, std::string> getZonePvpInfo(uint32_t zoneId) const {
        (void)getAreaName(zoneId);
        auto it = areaPvpCache_.find(zoneId);
        if (it == areaPvpCache_.end()) return {};
        constexpr uint32_t kAreaFlagArena     = 0x00000080;
        constexpr uint32_t kAreaFlagSanctuary = 0x00000800;
        constexpr uint32_t kTeamNone = 0, kTeamAlly = 2, kTeamHorde = 4, kTeamAny = 6;
        if (it->second.flags & kAreaFlagSanctuary) return {"sanctuary", ""};
        if (it->second.flags & kAreaFlagArena)     return {"arena", ""};
        const bool alliance = isPlayerAlliance();
        switch (it->second.team) {
            case kTeamAny:   return {"contested", ""};
            case kTeamAlly:  return {alliance ? "friendly" : "hostile", "Alliance"};
            case kTeamHorde: return {alliance ? "hostile" : "friendly", "Horde"};
            case kTeamNone:
            default:         return {};
        }
    }
    bool isHostileFaction(uint32_t factionTemplateId) const {
        if (const int atWar = factionAtWarLive(factionTemplateId); atWar >= 0) return atWar == 1;
        auto it = factionHostileMap_.find(factionTemplateId);
        return it != factionHostileMap_.end() ? it->second : true;
    }
    /// For a template whose faction carries a standing: 1 while the server
    /// says the player is at war with it, 0 when not, and -1 when it is not
    /// such a template or the standings have not arrived - the maps above,
    /// built from the starting standings, answer until then.
    int factionAtWarLive(uint32_t factionTemplateId) const;

private:
    // Dead: autoTargetAttacker moved to CombatHandler

    /// Everything of the world just left - entities, contacts, caches, the
    /// active character - shared by a disconnect and a logout to the
    /// character list, which differ only in whether the connection goes too.
    void clearWorldState();

    /**
     * Handle incoming packet from world server
     */
    void handlePacket(network::Packet& packet);
    void registerOpcodeHandlers();

    /// The opcodes this handler answers itself, grouped by subject.
    void registerCoreOpcodes();

    /// Everything else - inspects, auctions, calendars, voice, and a long tail
    /// that is consumed and ignored. Named for what it is.
    void registerRemainingOpcodes();

    /// The domain handlers registering their own. Disjoint from the two above,
    /// so the order between them carries no meaning.
    void registerDomainOpcodes();
    void registerSkipHandler(LogicalOpcode op);
    void registerHandler(LogicalOpcode op, void (GameHandler::*handler)(network::Packet&));
    void enqueueIncomingPacket(const network::Packet& packet);
    void enqueueIncomingPacketFront(network::Packet&& packet);
    void processQueuedIncomingPackets();

    /**
     * Handle SMSG_AUTH_CHALLENGE from server
     */
    void handleAuthChallenge(network::Packet& packet);

    /**
     * Handle SMSG_AUTH_RESPONSE from server
     */
    void handleAuthResponse(network::Packet& packet);

    /**
     * Handle SMSG_CHAR_ENUM from server
     */
    void handleCharEnum(network::Packet& packet);

    /**
     * Handle SMSG_CHARACTER_LOGIN_FAILED from server
     */
    void handleCharLoginFailed(network::Packet& packet);

    /**
     * Handle SMSG_LOGIN_VERIFY_WORLD from server
     */
    void handleLoginVerifyWorld(network::Packet& packet);

    /**
     * Handle SMSG_CLIENTCACHE_VERSION from server
     */
    void handleClientCacheVersion(network::Packet& packet);

    /**
     * Handle SMSG_TUTORIAL_FLAGS from server
     */
    void handleTutorialFlags(network::Packet& packet);

    /** Handle SMSG_NOTIFICATION (vanilla/classic server notification string) */
    void handleNotification(network::Packet& packet);

    /**
     * Handle SMSG_PONG from server
     */
    void handlePong(network::Packet& packet);

    // handleAttackStart, handleAttackStop, handleAttackerStateUpdate,
    // handleSpellDamageLog, handleSpellHealLog removed

    // handleSetForcedReactions - dispatched via CombatHandler


    // ---- Character creation handler ----
    void handleCharCreateResponse(network::Packet& packet);

    // ---- Instance lockout handler ----

    /**
     * Send CMSG_AUTH_SESSION to server
     */
    void sendAuthSession(const AuthChallengeData& challenge);

    /**
     * Generate random client seed
     */
    uint32_t generateClientSeed();

    /**
     * Change state with logging
     */
    void setState(WorldState newState);

    /**
     * Fail connection with reason
     */
    void fail(const std::string& reason);

    // Explicit service dependencies (owned by Application)
    GameServices& services_;

    // Domain handlers - each manages a specific concern extracted from GameHandler
    std::unique_ptr<QueryCacheHandler> queryCacheHandler_;
    std::unique_ptr<ChatHandler>      chatHandler_;
    std::unique_ptr<MovementHandler>  movementHandler_;
    std::unique_ptr<CombatHandler>    combatHandler_;
    std::unique_ptr<SpellHandler>     spellHandler_;
    std::unique_ptr<InventoryHandler> inventoryHandler_;
    std::unique_ptr<SocialHandler>    socialHandler_;
    std::unique_ptr<QuestHandler>     questHandler_;
    std::unique_ptr<WardenHandler>    wardenHandler_;
    std::unique_ptr<AccountDataHandler> accountDataHandler_;
    std::unique_ptr<ActionBarHandler> actionBarHandler_;
    std::unique_ptr<PetHandler> petHandler_;
    std::unique_ptr<ReputationHandler> reputationHandler_;
    std::unique_ptr<RealmTimeHandler> realmTimeHandler_;
    std::unique_ptr<WeatherHandler> weatherHandler_;
    std::unique_ptr<PlayerHandler> playerHandler_;

    // Opcode dispatch table - built once in registerOpcodeHandlers(), called by handlePacket()
    using PacketHandler = std::function<void(network::Packet&)>;
    std::unordered_map<LogicalOpcode, PacketHandler> dispatchTable_;

    // Opcode translation table (expansion-specific wire ↔ logical mapping)
    OpcodeTable opcodeTable_;

    // Update field table (expansion-specific field index mapping)
    UpdateFieldTable updateFieldTable_;

    // Packet parsers (expansion-specific binary format handling)
    std::unique_ptr<PacketParsers> packetParsers_;

    // Network
    std::unique_ptr<network::WorldConnection> socket;
    std::deque<network::Packet> pendingIncomingPackets_;

    // State
    WorldState state = WorldState::DISCONNECTED;
    float characterListWait_ = 0.0f;
    bool characterListRetried_ = false;

    // Authentication data
    std::vector<uint8_t> sessionKey;    // 40-byte session key from auth server
    std::string accountName;             // Account name
    uint32_t build = 12340;              // Client build (3.3.5a)
    uint32_t realmId_ = 0;               // Realm ID from auth REALM_LIST (used in WotLK AUTH_SESSION)
    // Where that session was made, for lastWorldLogin().
    std::string worldHost_;
    uint16_t worldPort_ = 0;
    std::string extensionHost_;
    uint16_t extensionPort_ = 0;
    /// See worldConnectionLost().
    bool worldConnectionLost_ = false;
    /// The refusals said since the connection was lost, one log line each.
    std::set<std::string> refusalsLogged_;
    uint32_t clientSeed = 0;             // Random seed generated by client

    // Characters
    std::vector<Character> characters;       // Character list from SMSG_CHAR_ENUM

    // Movement
    MovementInfo movementInfo;               // Current player movement state
    uint32_t movementTime = 0;               // Movement timestamp counter

    // Fall/jump tracking for movement packet correctness.
    // fallTime must be the elapsed ms since the FALLING flag was set; the server
    // uses it for fall-damage calculations and anti-cheat validation.

    // Entity tracking (delegated to WorldHandler)
    std::unique_ptr<WorldHandler> worldHandler_;

    // Chat (state lives in ChatHandler; callbacks remain here for cross-domain access)
    AddonEventCallback addonEventCallback_;
    AddonEventQueue pendingAddonEvents_;
    InterfaceCommand   interfaceCommand_;
    SpellIconPathResolver spellIconPathResolver_;
    IconPathResolver iconPathResolver_;
    ItemIconPathResolver itemIconPathResolver_;
    SpellDataResolver spellDataResolver_;
    RandomPropertyNameResolver randomPropertyNameResolver_;
    RandomStatResolver randomStatResolver_;

    // Heartbeat
    uint32_t pingSequence = 0;               // Ping sequence number (increments)
    float timeSinceLastPing = 0.0f;          // Time since last ping sent (seconds)
    float pingInterval = 30.0f;              // Ping interval (30 seconds)
    uint32_t lastLatency = 0;                // Last measured latency (milliseconds)
    std::chrono::steady_clock::time_point pingTimestamp_;  // Time CMSG_PING was sent
    /// When the instance boot timer runs out. Unset while none is running.
    std::optional<std::chrono::steady_clock::time_point> instanceBootDeadline_;

    // Player GUID and map
    uint64_t playerGuid = 0;
    uint32_t currentMapId_ = 0;
    bool hasHomeBind_ = false;
    uint32_t homeBindMapId_ = 0;
    uint32_t homeBindZoneId_ = 0;
    glm::vec3 homeBindPos_{0.0f};

    // ---- World state and faction initialization snapshots ----
    uint32_t worldStateMapId_ = 0;
    uint32_t worldStateZoneId_ = 0;
    // The active non-combat companion and the spell that called it. Re-casting
    // that spell dismisses rather than re-summons, which is how a companion is
    // put away: it has no aura to cancel from the buff bar, so this toggle is
    // the only way to send it back.
    std::unordered_map<uint32_t, uint32_t> worldStates_;

    // ---- Display state ----

    // Items, coin, books and the equipment of players in view are the
    // inventory's (InventoryHandler); none of it is kept here.
    /// The innkeeper that asked "make this your home?". SMSG_BINDER_CONFIRM
    /// names it and the answer has to go back to the same one.
    uint64_t binderGuid_ = 0;

    // Transport tracking
    std::unique_ptr<TransportManager> transportManager_;  // Transport movement manager
    uint32_t weaponProficiency_ = 0;  // bitmask from SMSG_SET_PROFICIENCY itemClass=2
    uint32_t armorProficiency_  = 0;  // bitmask from SMSG_SET_PROFICIENCY itemClass=4
    uint64_t pendingGameObjectInteractGuid_ = 0;
    // Owned fishing bobber whose bite animation has fired. Keeping this explicit
    // lets the UI target/reel it even while GAMEOBJECT_QUERY metadata is pending.
    uint64_t hookedFishingBobberGuid_ = 0;


    // Craft queue: seconds the expired cast bar has waited for SMSG_SPELL_GO
    // (which re-casts the next queued item) before giving up
    float craftCastGoGraceSec_ = 0.0f;

    // Mirror timers (0=fatigue, 1=breath, 2=feigndeath)
    MirrorTimer mirrorTimers_[3];





    uint64_t activeCharacterGuid_ = 0;
    Race playerRace_ = Race::HUMAN;

    // Barber shop
    bool barberShopOpen_ = false;

    // ---- Loot ----
    bool autoFaceTarget_ = false;
    bool autoSelfCast_ = true;

    struct PendingLootOpen {
        uint64_t guid = 0;
        float timer = 0.0f;
        uint8_t remainingAttempts = 1;
    };
    std::vector<PendingLootOpen> pendingGameObjectLootOpens_;
    // Tracks the last GO we sent CMSG_GAMEOBJ_USE to; used in handleSpellGo
    // to send CMSG_LOOT after a gather cast (mining/herbalism) completes.
    uint64_t lastInteractedGoGuid_ = 0;

    void performGameObjectInteractionNow(uint64_t guid);

    // Quest turn-in
    // (pending quest accept timeout state lives in QuestHandler)

    // Quest log. The entries and the selection both live in QuestHandler now;
    // the GameHandler selection copy this comment used to describe was dead -
    // written by setSelectedQuestLogIndex, read by nobody, while the getter went
    // to the QuestHandler - and has been removed.
    std::unordered_set<uint32_t> blobQuestIds_;
    bool pendingLoginQuestResync_ = false;
    float pendingLoginQuestResyncTimeout_ = 0.0f;


    // Faction hostility lookup (populated from FactionTemplate.dbc)
    std::unordered_map<uint32_t, bool> factionHostileMap_;
    std::unordered_map<uint32_t, bool> factionFriendlyMap_;
    std::unordered_map<uint32_t, uint32_t> factionTemplateRepList_;

    // Vehicle (WotLK): non-zero when player is seated in a vehicle
    uint32_t vehicleId_ = 0;

    /// Read once on the first ask, like the other DBC-backed caches here.
    mutable std::unordered_map<uint32_t, ExtendedCostEntry> extendedCostCache_;
    mutable bool extendedCostCacheLoaded_ = false;
    mutable std::unordered_map<uint32_t, ContinentBounds> continentBoundsCache_;
    std::vector<CurrencyType> currencyTypes_;
    bool currencyTypesLoaded_ = false;
    std::unordered_map<uint32_t, BattlemasterEntry> battlemasterList_;
    std::vector<BattlemasterEntry> battlegroundTypes_;  // arenas filtered out, id-ordered
    bool battlemasterListLoaded_ = false;
    bool glyphPropertiesLoaded_ = false;
    std::unordered_map<uint32_t, uint32_t> glyphSpellCache_;

    /// Sort moves still to send, one per tick. See sortBags.
    std::deque<Inventory::SwapOp> sortSwapQueue_;

    /// Chat lines still to send, one per tick. See queuePacedChat.
    std::deque<std::string> pacedChatQueue_;

    // Area name cache (lazy-loaded from WorldMapArea.dbc; maps AreaTable ID → display name)
    mutable std::unordered_map<uint32_t, std::string> areaNameCache_;
    /// AreaTable.dbc field 4 (flags) and field 28 (team), filled alongside the
    /// names. Field numbers and the constants below are read off AzerothCore's
    /// DBCStructure.h and DBCEnums.h rather than guessed.
    struct AreaPvpInfo { uint32_t flags = 0; uint32_t team = 0; };
    mutable std::unordered_map<uint32_t, AreaPvpInfo> areaPvpCache_;
    mutable bool areaNameCacheLoaded_ = false;
    void loadAreaNameCache() const;

    // Map metadata cache (lazy-loaded from Map.dbc).
    mutable std::unordered_map<uint32_t, std::string> mapNameCache_;
    mutable std::unordered_map<uint32_t, uint32_t> mapInstanceTypeCache_;
    mutable bool mapNameCacheLoaded_ = false;
    // (mapId, difficulty) -> encounter count, and the same map with no
    // difficulty-specific rows under a difficulty of 0xFFFFFFFF.
    mutable std::unordered_map<uint64_t, uint32_t> dungeonEncounterCounts_;
    mutable bool dungeonEncounterCacheLoaded_ = false;
    void loadMapNameCache() const;

    // LFG dungeon name cache (lazy-loaded from LFGDungeons.dbc; WotLK only)
    mutable std::unordered_map<uint32_t, std::string> lfgDungeonNameCache_;
    mutable std::vector<LfgDungeon> lfgDungeons_;
    mutable std::vector<Companion> mountSpells_;
    mutable std::vector<Companion> critterSpells_;
    mutable size_t companionsBuiltFromSpellCount_ = static_cast<size_t>(-1);
    uint32_t activeMountSpell_ = 0;
    uint32_t activeCritterSpell_ = 0;
    void rebuildCompanions() const;
    mutable bool lfgDungeonNameCacheLoaded_ = false;
    void loadLfgDungeonDbc() const;
    void preloadDBCCaches() const;

    // Callbacks
    WorldConnectSuccessCallback onSuccess;
    WorldConnectFailureCallback onFailure;
    bool pendingCharDeleteResponse_ = false;
    uint64_t pendingDeleteGuid_ = 0;
    float pendingDeleteTimer_ = 0.0f;
    bool pendingDeleteFallbackEnum_ = false;

    // Warden module download state
    enum class WardenState {
        WAIT_MODULE_USE,     // Waiting for first SMSG (MODULE_USE)
        WAIT_MODULE_CACHE,   // Sent MODULE_MISSING, receiving module chunks
        WAIT_HASH_REQUEST,   // Module received, waiting for HASH_REQUEST
        WAIT_CHECKS,         // Hash sent, waiting for check requests
    };

    // Pre-computed challenge/response entries from .cr file
    struct WardenCREntry {
        uint8_t seed[16];
        uint8_t reply[20];
        uint8_t clientKey[16];  // Encrypt key (client→server)
        uint8_t serverKey[16]; // Decrypt key (server→client)
    };
    // Module-specific check type opcodes [9]: MEM, PAGE_A, PAGE_B, MPQ, LUA, DRIVER, TIMING, PROC, MODULE
    uint8_t wardenCheckOpcodes_[9] = {};

    // ---- Silence on the world connection (watchWorldSilence) ----
    /// Seconds of listening, in the world, since the last packet arrived.
    float worldSilenceSeconds_ = 0.0f;
    /// Whether a ping has gone out to ask about the silence.
    bool worldSilenceProbed_ = false;

    // ---- Light override (SMSG_OVERRIDE_LIGHT) ----

    // ---- Player skills (the skills themselves are the spell book's) ----
    /// Headings the player has closed. Client-side, like the reputation ones,
    /// and saved with the rest of the character config for the same reason.
    std::unordered_set<uint32_t> collapsedSkillCategories_;
    FaceCameraProvider faceCameraProvider_;
    bool suppressMeleeSwingAnim_ = false;
    // lastMeleeSwingMs_ moved to CombatHandler
    /// PageTextMaterial.dbc is seven rows and is read once on the first ask,
    /// like the other DBC-backed caches here.
    mutable std::unordered_map<uint32_t, std::string> pageTextMaterialNames_;
    mutable bool pageTextMaterialsLoaded_ = false;
    /// Languages.dbc, read once on the first ask like the caches beside it.
    mutable std::unordered_map<uint32_t, std::string> languageNames_;
    mutable bool languageNamesLoaded_ = false;
    // Death Knight runes (class 6): slots 0-1=Blood, 2-3=Unholy, 4-5=Frost initially
    std::array<RuneSlot, 6> playerRunes_ = [] {
        std::array<RuneSlot, 6> r{};
        r[0].type = r[1].type = RuneType::Blood;
        r[2].type = r[3].type = RuneType::Unholy;
        r[4].type = r[5].type = RuneType::Frost;
        return r;
    }();
    // ---- Talent wipe confirm dialog ----
    // (talent wipe confirm state lives in SpellHandler)
    // ---- Pet talent respec confirm dialog ----
    // (pet unlearn confirm state lives in SpellHandler)

    // ---- Equipment sets (SMSG_EQUIPMENT_SET_LIST) ----
    struct EquipmentSet {
        uint64_t setGuid = 0;
        uint32_t setId = 0;
        std::string name;
        std::string iconName;
        uint32_t ignoreSlotMask = 0;
        std::array<uint64_t, 19> itemGuids{};
    };

    // ---- UI error frame callback ----

    // ---- Quest completion callback ----

    // ---- GM Ticket state (SMSG_GMTICKET_GETTICKET / SMSG_GMTICKET_SYSTEMSTATUS) ----
    std::string gmTicketText_;               ///< Text of the open ticket (from SMSG_GMTICKET_GETTICKET)
    float       gmTicketWaitHours_ = 0.0f;  ///< Server-estimated wait time in hours
    bool        gmSupportAvailable_ = true; ///< GM support system online (SMSG_GMTICKET_SYSTEMSTATUS)

    /// The conversation last asked for, until the server answers it.
    ///
    /// A server that refuses one says nothing - out of reach, a creature that
    /// regards the player as Unfriendly, one that is busy - and a refusal
    /// looked exactly like a click that never happened. Held so a hello with
    /// no answer can say what it was sent from. See interactWithNpc.
    struct PendingNpcHello {
        uint64_t guid = 0;
        std::string name;
        std::chrono::steady_clock::time_point sentAt{};
        float distance = -1.0f;
        int reaction = 0;
        uint32_t npcFlags = 0;
    };
    PendingNpcHello pendingNpcHello_;
    /// The server answered a conversation: whatever it said, the hello got
    /// through. Called from the dispatch for every packet that opens an NPC's
    /// window.
    void noteNpcAnswered() { pendingNpcHello_.guid = 0; }

    /// What this hears from its own domains. Last, so it ends before anything
    /// a handler uses goes.
    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace game
} // namespace wowee
