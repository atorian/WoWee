#include "game/game_handler.hpp"
#include "game/game_data.hpp"
#include "title/services/titles.hpp"
#include "reputation/domain/records.hpp"
#include "reputation/services/reputation.hpp"
#include "core/event_bus.hpp"
#include "game/game_events.hpp"
#include <stdexcept>
#include "game/reputation_standing.hpp"

#include <set>

#include "game/item_text.hpp"
#include "game/game_utils.hpp"
#include "game/chat_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/combat_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/social_handler.hpp"
#include "game/account_data_handler.hpp"
#include "game/realm_time_handler.hpp"
#include "game/weather_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/warden_handler.hpp"
#include "game/warden_crypto.hpp"
#include "game/warden_memory.hpp"
#include "game/warden_module.hpp"
#include "game/packet_parsers.hpp"
#include "game/transport_manager.hpp"
#include "game/opcodes.hpp"
#include "game/update_field_table.hpp"
#include "game/expansion_profile.hpp"
#include "pipeline/dbc_layout.hpp"
#include "network/world_connection.hpp"
#include "network/packet.hpp"
#include "auth/crypto.hpp"
#include "core/coordinates.hpp"
#include "core/config_paths.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "game/protocol_constants.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <random>
#include <zlib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <openssl/sha.h>
#include <openssl/hmac.h>

namespace wowee {
namespace game {

void GameHandler::dispatchAddonEvents() {
    pendingAddonEvents_.dispatch(
        [this](const auto& name, const auto& args) {
            if (addonEventCallback_) addonEventCallback_(name, args);
        },
        [this](const auto& code) {
            if (interfaceCommand_) interfaceCommand_(code);
        });
}

// Registration helpers for common dispatch table patterns
void GameHandler::registerSkipHandler(LogicalOpcode op) {
    dispatchTable_[op] = [](network::Packet& packet) { packet.skipAll(); };
}
void GameHandler::registerHandler(LogicalOpcode op, void (GameHandler::*handler)(network::Packet&)) {
    dispatchTable_[op] = [this, handler](network::Packet& packet) { (this->*handler)(packet); };
}
GameHandler::GameHandler(GameServices& services)
    : services_(services) {
    if (!services_.events) {
        throw std::invalid_argument("GameHandler needs GameServices::events");
    }
    if (!services_.emotes) {
        throw std::invalid_argument("GameHandler needs GameServices::emotes");
    }
    if (!services_.statTables) {
        throw std::invalid_argument("GameHandler needs GameServices::statTables");
    }
    if (!services_.gameData) {
        throw std::invalid_argument("GameHandler needs GameServices::gameData");
    }
    if (!services_.cvars) {
        throw std::invalid_argument("GameHandler needs GameServices::cvars");
    }
    if (!services_.makeConnection) {
        throw std::invalid_argument("GameHandler needs GameServices::makeConnection");
    }
    LOG_DEBUG("GameHandler created");

    setActiveOpcodeTable(&opcodeTable_);
    setActiveUpdateFieldTable(&updateFieldTable_);

    // Initialize packet parsers (WotLK default, may be replaced for other expansions)
    packetParsers_ = std::make_unique<WotlkPacketParsers>();

    // Initialize transport manager
    transportManager_ = std::make_unique<TransportManager>();

    // Initialize Warden module manager

    // Initialize domain handlers
    queryCacheHandler_ = std::make_unique<QueryCacheHandler>(*this, *services_.events, *this);
    worldHandler_ = std::make_unique<WorldHandler>(*this, *this, *services_.events, *this, *services_.emotes);
    chatHandler_      = std::make_unique<ChatHandler>(*this, *this, *services_.events, *this,
                                                      *services_.cvars, *services_.emotes);
    movementHandler_  = std::make_unique<MovementHandler>(*this, *this, *services_.events, *chatHandler_,
                                                          *services_.gameData, *this, movementInfo,
                                                          services_.clientMover);
    combatHandler_    = std::make_unique<CombatHandler>(*this, *this, *services_.events, *chatHandler_,
                                                        *services_.cvars, *this);
    socialHandler_    = std::make_unique<SocialHandler>(*this, *this, *services_.events, *chatHandler_,
                                                        *this, *services_.cvars);
    questHandler_     = std::make_unique<QuestHandler>(*this, *this, *services_.events, *chatHandler_,
                                                       *this, *services_.gameData);
    // Built from what they use rather than from this class (AGENTS.md, "A new
    // domain handler").
    spellHandler_ = std::make_unique<SpellHandler>(
        *this, *this, *services_.events, *chatHandler_, *this, *services_.cvars,
        *services_.gameData);
    inventoryHandler_ = std::make_unique<InventoryHandler>(
        *this, *this, *services_.events, *chatHandler_, *this, *services_.cvars,
        *services_.gameData);
    accountDataHandler_ = std::make_unique<AccountDataHandler>(
        *this, *this, *services_.events, services_.accountDataDir);
    actionBarHandler_ = std::make_unique<ActionBarHandler>(*this, *this, *services_.events, *this);
    wardenHandler_ = std::make_unique<WardenHandler>(
        *this, *this, *services_.events, services_.assetManager, services_.expansionRegistry);
    petHandler_ = std::make_unique<PetHandler>(*this, *this, *services_.events, *chatHandler_, *this);
    reputationHandler_ = std::make_unique<ReputationHandler>(*this, *services_.events, *chatHandler_,
                                                             *this);
    realmTimeHandler_ = std::make_unique<RealmTimeHandler>(*services_.events, services_.steadyNow);
    weatherHandler_ = std::make_unique<WeatherHandler>(
        *services_.events, *this, *chatHandler_,
        [] { return isClassicLikeExpansion() ? WeatherNumbering::Classic : WeatherNumbering::WeatherState; });
    playerHandler_ = std::make_unique<PlayerHandler>(*this, *this, *services_.events, *chatHandler_, *this,
                                                    *services_.statTables);

    // The character's own settings, read once its spells have arrived - the
    // first thing the realm sends of a character entering the world.
    subscriptions_.push_back(eventBus().subscribe<events::CharacterSpellsArrived>(
        [this](const events::CharacterSpellsArrived&) { loadCharacterConfig(); }));
    // The tracked quests are kept with the character's settings.
    subscriptions_.push_back(eventBus().subscribe<events::QuestTrackingChanged>(
        [this](const events::QuestTrackingChanged&) { saveCharacterConfig(); }));
    // Opening a game object waits on the cast that opens it.
    subscriptions_.push_back(eventBus().subscribe<events::PlayerCastEnded>(
        [this](const events::PlayerCastEnded& e) { gameObjectCastEnded(e); }));

    // The action bar starts empty. What is on it is the server's to say -
    // SMSG_ACTION_BUTTONS at every login, which for a new character carries
    // the race and class defaults - and a layout guessed here only showed
    // until that arrived, and then wherever it did not overwrite.

    // Build the opcode dispatch table (replaces switch(*logicalOp) in handlePacket)
    registerOpcodeHandlers();
}

GameHandler::~GameHandler() {
    disconnect();
}

void GameHandler::setPacketParsers(std::unique_ptr<PacketParsers> parsers) {
    packetParsers_ = std::move(parsers);
}

bool GameHandler::connect(const std::string& host,
                          uint16_t port,
                          const std::vector<uint8_t>& sessionKey,
                          const std::string& accountName,
                          uint32_t build,
                          uint32_t realmId,
                          const std::string& extensionServerHost,
                          uint16_t extensionServerPort) {

    if (sessionKey.size() != 40) {
        LOG_ERROR("Invalid session key size: ", sessionKey.size(), " (expected 40)");
        fail("Invalid session key");
        return false;
    }

    LOG_INFO("========================================");
    LOG_INFO("   CONNECTING TO WORLD SERVER");
    LOG_INFO("========================================");
    LOG_INFO("Host: ", host);
    LOG_INFO("Port: ", port);
    LOG_INFO("Account: ", accountName);
    LOG_INFO("Build: ", build);

    // Session-scoped compatibility: drain old work and unload its adapter
    // before another server can see either its library or its PE memory.
    if (!wardenHandler_->beginSession(
            extensionServerHost.empty() ? host : extensionServerHost,
            extensionServerHost.empty() ? port : extensionServerPort, build)) {
        fail(wardenHandler_->extensionError());
        return false;
    }

    // Store authentication data
    this->sessionKey = sessionKey;
    this->accountName = accountName;
    this->build = build;
    this->realmId_ = realmId;
    worldHost_ = host;
    worldPort_ = port;
    extensionHost_ = extensionServerHost;
    extensionPort_ = extensionServerPort;
    accountDataHandler_->beginSession(accountName, host);

    // The session key is the shared secret the world handshake proves knowledge
    // of and the header cipher is keyed from: anyone holding these 40 bytes can
    // authenticate as this account and decrypt its traffic. It must never reach
    // a log, which is a file users are routinely asked to attach to a bug
    // report. Its length is the only part of it that diagnoses anything.
    LOG_INFO("GameHandler session key received (", sessionKey.size(), " bytes)");
    // Generate random client seed
    this->clientSeed = generateClientSeed();
    LOG_DEBUG("Generated client seed: 0x", std::hex, clientSeed, std::dec);

    // The connection the client hands over: TCP in the client, a recording
    // stand-in in a test.
    socket = services_.makeConnection();

    // Set up packet callback
    socket->setPacketCallback([this](const network::Packet& packet) {
        enqueueIncomingPacket(packet);
    });

    // Connect to world server, off the main thread: this waited in the frame
    // for up to ten seconds when the realm did not answer. update() finishes
    // it; until then the state is CONNECTING, which the connection screen
    // shows and gives a deadline of its own.
    setState(WorldState::CONNECTING);
    socket->beginConnect(host, port, 10);
    return true;
}

bool GameHandler::connect(const WorldLogin& login) {
    return connect(login.host, login.port, login.sessionKey, login.account, login.build,
                   login.realmId, login.extensionHost, login.extensionPort);
}

WorldLogin GameHandler::lastWorldLogin() const {
    return WorldLogin{worldHost_, worldPort_, sessionKey, accountName, build, realmId_,
                      extensionHost_, extensionPort_};
}

bool GameHandler::worldCommandAllowed(const char* command) {
    if (isInWorld()) return true;
    if (!worldConnectionLost_) return false;
    addUIError("No connection to the server.");
    if (refusalsLogged_.insert(command).second) {
        LOG_WARNING("Refused ", command, ": the world connection is lost and being restored; "
                    "nothing is kept to send when it is back");
    }
    return false;
}

void GameHandler::disconnect() {
    if (socket) {
        socket->disconnect();
        socket.reset();
    }
    // Warden state is WardenHandler's; this used to clear a second copy of it
    // that nothing ever filled.
    wardenHandler_->reset();
    clearWorldState();
    setState(WorldState::DISCONNECTED);
    LOG_INFO("Disconnected from world server");
}

void GameHandler::returnToCharacterSelect() {
    clearWorldState();
    setState(WorldState::READY);
    LOG_INFO("Left the world; back to the character list on the same connection");
    // Asked for here, not left to the character screen: it asks only when it
    // has no list, and it still had the one from before entering the world -
    // so nothing asked, the handler stayed READY, and selecting a character
    // was refused for not having a list. This also clears the stale one.
    requestCharacterList();
}

void GameHandler::clearWorldState() {
    movementHandler_->cancelPendingWorldTransfer();
    activeCharacterGuid_ = 0;
    if (services_.titlesFeed) services_.titlesFeed->leftWorld();
    movementHandler_->clearTransportAttachments();
    pendingIncomingPackets_.clear();
    // Say every entity left view so the renderer releases M2/character model resources.
    for (const auto& [guid, entity] : worldHandler_->getEntityManager().getEntities()) {
        if (guid == playerGuid) continue;
        if (entity->getType() == ObjectType::UNIT)
            eventBus().publish(events::CreatureDespawned{guid});
        else if (entity->getType() == ObjectType::PLAYER)
            eventBus().publish(events::PlayerDespawned{guid});
        else if (entity->getType() == ObjectType::GAMEOBJECT)
            eventBus().publish(events::GameObjectDespawned{guid});
    }
    inventoryHandler_->leaveWorld();
    spellHandler_->clearUnitCastStates();
    spellHandler_->clearUnitAurasCache();
    combatHandler_->clearCombatText();
    worldHandler_->clearAll();
    queryCacheHandler_->clear();
}

void GameHandler::resetDbcCaches() {
    movementHandler_->forgetAreaTriggerTable();
    // The spell, skill, talent and taxi tables live in the sub-handlers, and
    // only there.
    spellHandler_->resetSpellDbcCaches();
    movementHandler_->resetTaxiDbcCache();
    // Clear the AssetManager DBC file cache so that expansion-specific DBCs
    // (CharSections, ItemDisplayInfo, etc.) are reloaded from the new expansion's
    // MPQ files instead of returning stale data from a previous session/expansion.
    auto* am = services_.assetManager;
    if (am) {
        am->clearDBCCache();
    }
    LOG_INFO("GameHandler: DBC caches cleared for expansion switch");
}

bool GameHandler::isConnected() const {
    return socket && socket->isConnected();
}

void GameHandler::send(const network::Packet& packet) {
    if (socket) socket->send(packet);
}

ActionCooldown GameHandler::spellCooldown(uint32_t spellId) const {
    const float remaining = spellHandler_->getSpellCooldown(spellId);
    if (remaining <= 0.0f) return {};
    return {.remaining = remaining, .total = spellHandler_->getSpellCooldownTotal(spellId)};
}

std::optional<uint32_t> GameHandler::playerFieldBytes() const {
    const uint16_t index = fieldIndex(UF::PLAYER_FIELD_BYTES);
    if (index == 0xFFFF) return std::nullopt;
    const auto player = worldHandler_->getEntityManager().getEntity(playerGuid);
    if (!player || !player->hasField(index)) return std::nullopt;
    return player->getField(index);
}

std::array<uint32_t, 3> GameHandler::spellEffects(uint32_t spellId) const {
    const SpellNameEntry* spell = spellHandler_->findSpell(spellId);
    if (!spell) return {};
    return {spell->effectIds[0], spell->effectIds[1], spell->effectIds[2]};
}

uint32_t GameHandler::classSkillLineOf(uint32_t spellId) {
    // SkillLine.dbc category 7 is a class's own lines - a talent tree, a spec.
    static constexpr uint32_t kSkillLineCategoryClass = 7;
    const uint32_t line = spellHandler_->skillLineOfSpell(spellId);
    if (line == 0 || spellHandler_->skillCategory(line) != kSkillLineCategoryClass) return 0;
    return line;
}

void GameHandler::setFacing(float canonicalYaw) {
    movementHandler_->setOrientation(canonicalYaw);
    sendMovement(Opcode::MSG_MOVE_SET_FACING);
}

uint32_t GameHandler::itemEntryByGuid(uint64_t itemGuid) const {
    return inventoryHandler_->getItemEntryByGuid(itemGuid);
}

std::optional<HomeBind> GameHandler::homeBind() const {
    return playerHandler_->homeBind();
}

void GameHandler::noteItemCastSpell(uint32_t spellId) {
    spellHandler_->noteGroundCastSpell(spellId);
}

void GameHandler::stopMovingForCast() {
    // Bandages are cast through CMSG_USE_ITEM and therefore bypass
    // SpellHandler::castSpell(), which normally sends a stop before a timed cast.
    // Explicitly synchronize the stationary state so the server cannot retain a
    // stale start-forward/strafe/turn state after the character has visually stopped.
    const uint32_t horizontalMask =
        static_cast<uint32_t>(MovementFlags::FORWARD) |
        static_cast<uint32_t>(MovementFlags::BACKWARD) |
        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
        static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT);
    const uint32_t turnMask =
        static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
        static_cast<uint32_t>(MovementFlags::TURN_RIGHT);
    movementInfo.flags &= ~(horizontalMask | turnMask);

    sendMovement(Opcode::MSG_MOVE_STOP);
    sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
    sendMovement(Opcode::MSG_MOVE_STOP_TURN);
    sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
}

bool GameHandler::gameObjectLootPending(uint64_t lootGuid) const {
    if (lastInteractedGoGuid_ == 0 || lootGuid != lastInteractedGoGuid_) return false;
    return (spellHandler_->isCasting() && spellHandler_->getCurrentCastSpellId() != 0) || hasPendingGameObjectLootOpen(lootGuid);
}

bool GameHandler::gameObjectLootOpened(uint64_t lootGuid) {
    const bool interacted = lastInteractedGoGuid_ != 0 && lootGuid == lastInteractedGoGuid_;
    if (interacted) lastInteractedGoGuid_ = 0;
    clearPendingGameObjectLootOpen(lootGuid);
    return interacted;
}

bool GameHandler::npcDialogOpen() const {
    return questHandler_->isQuestDetailsOpen() || questHandler_->isGossipWindowOpen() ||
           questHandler_->isQuestRequestItemsOpen() || questHandler_->isQuestOfferRewardOpen();
}

bool GameHandler::nameUnitInView(uint64_t guid, const std::string& name) {
    auto unit = std::dynamic_pointer_cast<Unit>(worldHandler_->getEntityManager().getEntity(guid));
    if (!unit) return false;
    unit->setName(name);
    return true;
}

bool GameHandler::questWantsItem(uint32_t itemId) const {
    for (const auto& quest : questHandler_->getQuestLog()) {
        for (const auto& obj : quest.itemObjectives) {
            if (obj.itemId == itemId && obj.required > 0) return true;
        }
    }
    return false;
}

ActionCooldown GameHandler::itemCooldown(uint32_t itemId) const {
    // An item's cooldown is that of the spell it casts when used - a potion,
    // a trinket. SMSG_ITEM_COOLDOWN keeps it under that spell.
    const auto* info = inventoryHandler_->getItemInfo(itemId);
    if (!info || !info->valid) return {};
    for (const auto& spell : info->spells) {
        if (spell.spellId == 0) continue;
        if (const ActionCooldown cooldown = spellCooldown(spell.spellId); !cooldown.ready()) {
            return cooldown;
        }
    }
    return {};
}

void GameHandler::updateNetworking() {
    // One queued sort move per tick. A sort is dozens of swaps and the server
    // drops a burst of them, so they go out at the rate anything else does.
    inventoryHandler_->sendNextSortMove();

    // Reset per-tick monster-move budget tracking (Classic flood protection).
    movementHandler_->monsterMovePacketsThisTickRef() = 0;
    movementHandler_->monsterMovePacketsDroppedThisTickRef() = 0;

    // One queued chat line per tick, for the same reason and at the same rate
    // as the sort moves above.
    chatHandler_->sendNextPacedLine();

    // Anything still waiting on a name query that never came back. After the
    // socket below would be a tick later than it needs to be; before it means
    // a line waits one extra tick at most.
    chatHandler_->expireChatAwaitingName();

    // Update socket (processes incoming data and triggers callbacks)
    if (socket) {
        auto socketStart = std::chrono::steady_clock::now();
        socket->update();
        float socketMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - socketStart).count();
        if (socketMs > 3.0f) {
            LOG_WARNING("SLOW socket->update: ", socketMs, "ms");
        }
    }

    {
        auto packetStart = std::chrono::steady_clock::now();
        processQueuedIncomingPackets();
        float packetMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - packetStart).count();
        // Above the catch-up budget plus a handler overrunning it: anything
        // under that is the budget being spent as intended.
        if (packetMs > 20.0f) {
            LOG_WARNING("SLOW queued packet handling: ", packetMs, "ms");
        }
    }

    // Detect server-side disconnect (socket closed during update)
    if (socket && !socket->isConnected() && state != WorldState::DISCONNECTED) {
        if (movementHandler_->hasPendingWorldTransfer() ||
            (pendingIncomingPackets_.empty() && !worldHandler_->hasPendingUpdateObjectWork())) {
            LOG_WARNING("Server closed connection in state: ", worldStateName(state));
            // The client releases the world after submitting the frame. Keep
            // entities and the closed connection alive until that command.
            loseWorldConnection();
            return;
        }
        LOG_DEBUG("World socket closed with ", pendingIncomingPackets_.size(),
                  " queued packet(s) and update-object batch(es) pending dispatch");
    }

}

void GameHandler::watchWorldSilence(float deltaTime) {
    // Only where the server has reason to talk: in the world, and not between
    // SMSG_NEW_WORLD and the client's acknowledgement, when it waits for us.
    if (state != WorldState::IN_WORLD || !socket || !socket->isConnected() ||
        movementHandler_->hasPendingWorldTransfer()) {
        worldSilenceSeconds_ = 0.0f;
        worldSilenceProbed_ = false;
        return;
    }
    worldSilenceSeconds_ += std::clamp(deltaTime, 0.0f, game::WORLD_SILENCE_MAX_FRAME_SEC);
    if (!worldSilenceProbed_ && worldSilenceSeconds_ >= game::WORLD_SILENCE_PROBE_SEC) {
        // A quiet zone can be quiet this long; a ping is always answered.
        worldSilenceProbed_ = true;
        LOG_WARNING("World server silent for ", worldSilenceSeconds_,
                    " s; asking with a ping whether the connection is still there");
        sendPing();
        return;
    }
    if (worldSilenceProbed_ &&
        worldSilenceSeconds_ >= game::WORLD_SILENCE_PROBE_SEC + game::WORLD_SILENCE_ANSWER_SEC) {
        LOG_ERROR("World connection lost: nothing from the server for ", worldSilenceSeconds_,
                  " s, the ping asking about it unanswered; the socket never said so");
        loseWorldConnection();
    }
}

void GameHandler::loseWorldConnection() {
    if (socket) socket->disconnect();
    if (state == WorldState::IN_WORLD) worldConnectionLost_ = true;
    setState(WorldState::DISCONNECTED);
    worldSilenceSeconds_ = 0.0f;
    worldSilenceProbed_ = false;
    eventBus().publish(events::Disconnected{playerGuid});
}

void GameHandler::updateTaxiAndMountState(float deltaTime) {
    // The flight itself - path, mount, landing and the activation timeout - is
    // MovementHandler's, and advances there.
    movementHandler_->updateClientTaxi(deltaTime);

    movementHandler_->updatePlayerTransportHold(deltaTime);

    movementHandler_->reconcileMount();
}

void GameHandler::updateEntityInterpolation(float deltaTime) {
// Advance every server-tracked unit once per frame. This is intentionally not
// selected-target or distance gated: the spatial query is indexed by each
// mover's latest destination, so a creature visibly beside the player could be
// excluded while its destination was outside the query radius. Selecting that
// creature then put it on the unconditional path and made it snap forward.
// Entity interpolation is inexpensive; rendering retains its own distance
// culling and remains the correct place to skip costly visual work.
for (const auto& [guid, entity] : worldHandler_->getEntityManager().getEntities()) {
    (void)guid;
    if (entity && entity->isUnit()) {
        entity->updateMovement(deltaTime, services_.movementCollision);
    }
}
}

void GameHandler::updateTimers(float deltaTime) {
    spellHandler_->updateTimers(deltaTime);
    // Stale item queries, the auction search cooldown, the loot money fallback
    // and the equipment inspects - each with the state it counts down.
    inventoryHandler_->update(deltaTime);

    // The quest giver marks' requery, unconfirmed accepts and the login's
    // rebuild of the log.
    questHandler_->update(deltaTime);

    // The corpse-proximity crossing, for FrameXML's reclaim prompt.
    combatHandler_->announceCorpseRange();

    for (auto it = pendingGameObjectLootOpens_.begin(); it != pendingGameObjectLootOpens_.end();) {
        it->timer -= deltaTime;
        if (it->timer <= 0.0f) {
            if (isInWorld()) {
                // Avoid sending CMSG_LOOT while a timed cast is active (e.g. gathering).
                // handleSpellGo will trigger loot after the cast completes.
                if (spellHandler_->isCasting() && spellHandler_->getCurrentCastSpellId() != 0) {
                    it->timer = 0.20f;
                    ++it;
                    continue;
                }
                // The click may have landed before the object's metadata did.
                // A fishing school is never opened by looting it.
                if (isFishingHoleGameObject(it->guid)) {
                    it = pendingGameObjectLootOpens_.erase(it);
                    continue;
                }
                lootTarget(it->guid);
            }
            if (it->remainingAttempts > 1) {
                --it->remainingAttempts;
                it->timer = 0.75f;
                ++it;
            } else {
                it = pendingGameObjectLootOpens_.erase(it);
            }
        } else {
            ++it;
        }
    }

    // Periodically re-query names for players whose initial CMSG_NAME_QUERY was
    // lost (server didn't respond) or whose entity was recreated while the query
    // was still pending. Runs every 5 seconds to keep overhead minimal.
    if (isInWorld()) {
        static float nameResyncTimer = 0.0f;
        nameResyncTimer += deltaTime;
        if (nameResyncTimer >= 5.0f) {
            nameResyncTimer = 0.0f;
            for (const auto& [guid, entity] : worldHandler_->getEntityManager().getEntities()) {
                if (!entity || entity->getType() != ObjectType::PLAYER) continue;
                if (guid == playerGuid) continue;
                auto player = std::static_pointer_cast<Player>(entity);
                if (!player->getName().empty()) continue;
                // Player entity exists with empty name and no pending query - resend.
                queryCacheHandler_->queryPlayerName(guid);
            }
        }
    }

}

void GameHandler::update(float deltaTime) {
    updateNetwork(deltaTime);
    updateGame(deltaTime);
}

void GameHandler::updateNetwork(float deltaTime) {
    // The char-create callback used to be deferred to here, out of the packet
    // handler and so out of any render pass. It is not any more: the
    // SMSG_CHAR_CREATE handler publishes CharacterCreateAnswered itself, and nothing
    // had set the flag this waited on for long enough that the three members
    // behind it were never written at all. A block that cannot run, guarding a
    // deferral that no longer happens, reads as though both still do.
    if (!socket || state == WorldState::DISCONNECTED) {
        return;
    }

    if (state == WorldState::CONNECTING) {
        switch (socket->pollConnect()) {
            case net::AsyncConnect::Status::Connected:
                setState(WorldState::CONNECTED);
                LOG_INFO("Connected to world server, waiting for SMSG_AUTH_CHALLENGE...");
                break;
            case net::AsyncConnect::Status::Failed:
                fail("Could not reach the world server (" + socket->connectError() + ")");
                return;
            default:
                return;  // still connecting; nothing else runs without a connection
        }
    }

    updateNetworking();
    if (socket && state != WorldState::DISCONNECTED) watchWorldSilence(deltaTime);
    if (!socket || state == WorldState::DISCONNECTED ||
        movementHandler_->hasPendingWorldTransfer()) return;

    // Sends a Warden response built off the main thread. Nothing else does:
    // without this the server's check goes unanswered and it drops the
    // connection a few seconds later.
    wardenHandler_->update(deltaTime);
    const auto why = wardenHandler_->extensionError();
    if (!why.empty()) {
        if (socket) socket->disconnect();
        fail(why);
        return;
    }

    // Consume the adapter hint even if enumeration has already succeeded, so
    // it cannot trigger a stale retry after character creation/deletion.
    const bool retryAfterServerChecks = wardenHandler_->takeCharacterListRetryRequest();
    const bool retryAtProtocolCheckpoint = wardenHandler_->takeEarlyCharacterListRetryRequest();
    if (retryAtProtocolCheckpoint && !retryAfterServerChecks && state == WorldState::CHAR_LIST_REQUESTED &&
        socket->isConnected() && !characterListRetried_) {
        LOG_INFO("Server protocol checkpoint after ", characterListWait_,
                 "s - querying characters while initialization checks continue");
        socket->send(CharEnumPacket::build());
        // An early query may be discarded. Preserve the final-boundary/timeout retry.
    }
    // Some realms discard the first enumeration during setup. Retry once at a
    // verified protocol boundary; keep the timer as a fallback for other realms.
    if (state == WorldState::CHAR_LIST_REQUESTED && socket->isConnected() &&
        !characterListRetried_) {
        characterListWait_ += std::max(0.0f, deltaTime);
        if (retryAfterServerChecks || characterListWait_ >= 30.0f) {
            characterListRetried_ = true;
            if (retryAfterServerChecks)
                LOG_INFO("Server initialization checks ended after ", characterListWait_,
                         "s - retrying CMSG_CHAR_ENUM once");
            else
                LOG_INFO("No character list after 30s - retrying CMSG_CHAR_ENUM once");
            socket->send(CharEnumPacket::build());
        }
    }

    // Fallback for CMSG_CHAR_DELETE with no server response: if the server
    // doesn't send SMSG_CHAR_DELETE within 3 seconds, re-request the character
    // list.  Some server cores silently process the delete without responding.
    if (pendingCharDeleteResponse_) {
        pendingDeleteTimer_ += deltaTime;
        if (pendingDeleteTimer_ >= 3.0f) {
            LOG_WARNING("No SMSG_CHAR_DELETE response after 3s - requesting character list to verify");
            pendingCharDeleteResponse_ = false;
            pendingDeleteFallbackEnum_ = true;
            requestCharacterList();
        }
    }

    // After the fallback SMSG_CHAR_ENUM has been processed, check if the
    // character was actually removed and fire the delete callback.
    if (pendingDeleteFallbackEnum_ && state == WorldState::CHAR_LIST_RECEIVED) {
        pendingDeleteFallbackEnum_ = false;
        uint64_t deletedGuid = pendingDeleteGuid_;
        pendingDeleteGuid_ = 0;
        bool found = false;
        for (const auto& ch : characters) {
            if (ch.guid == deletedGuid) { found = true; break; }
        }
        bool deleted = !found;
        LOG_INFO("Char delete fallback: GUID 0x", std::hex, deletedGuid, std::dec,
                 deleted ? " was deleted" : " still exists");
        std::string msg;
        if (deleted) {
            msg = "Character deleted.";
        } else {
            msg = "Delete failed: the server did not respond. "
                  "This usually happens if you recently logged out - "
                  "wait 20-30 seconds and try again.";
        }
        eventBus().publish(events::CharacterDeleteAnswered{deleted, msg});
    }

}

void GameHandler::updateGame(float deltaTime) {
    if (!socket || state == WorldState::CONNECTING || state == WorldState::DISCONNECTED ||
        state == WorldState::FAILED || movementHandler_->hasPendingWorldTransfer()) return;

    // Validate target still exists
    if (const uint64_t target = combatHandler_->getTargetGuid();
        target != 0 && !worldHandler_->getEntityManager().hasEntity(target)) {
        combatHandler_->clearTarget();
    }

    // Auto-follow: refresh the followed unit's position, or stop if it is gone.
    movementHandler_->updateFollow(deltaTime);

    // Entering and leaving combat is fired from the update block that carries
    // UNIT_FLAG_IN_COMBAT, in WorldHandler - not from here.
    //
    // Both places used to fire it, so every fight announced itself twice: the
    // combat text showed "Entering Combat" and then "Entering Combat" again,
    // and the same on the way out. The two do not even agree on the question.
    // This one asked isInCombat(), which is `autoAttacking_ ||
    // !hostileAttackers_.empty()` - the client's own inference from what it has
    // seen swing - while the other reads the flag the *server* sets, which is
    // what PLAYER_REGEN_DISABLED means in WoW and what decides whether a panel
    // may be changed. Two sources, two edges, at slightly different moments.

    updateTimers(deltaTime);

    // Send periodic heartbeat if in world
    if (state == WorldState::IN_WORLD) {
        timeSinceLastPing += deltaTime;
        movementHandler_->timeSinceLastMoveHeartbeatRef() += deltaTime;

        const float currentPingInterval =
            (isPreWotlk()) ? game::CLASSIC_PING_INTERVAL_SEC : pingInterval;
        if (timeSinceLastPing >= currentPingInterval) {
            if (socket) {
                sendPing();
            }
            timeSinceLastPing = 0.0f;
        }

        const bool classicLikeCombatSync =
            (combatHandler_->hasAutoAttackIntent()) && (isPreWotlk());
        const bool onRealTaxiFlight = movementHandler_->isOnTaxiFlight();
        const bool classicLikeStationaryCombatSync =
            classicLikeCombatSync &&
            !onRealTaxiFlight &&
            (movementInfo.flags & kLocomotionFlags) == 0;
        float heartbeatInterval = onRealTaxiFlight
                                      ? game::HEARTBEAT_INTERVAL_TAXI
                                      : (classicLikeStationaryCombatSync ? game::HEARTBEAT_INTERVAL_STATIONARY_COMBAT
                                                                         : (classicLikeCombatSync ? game::HEARTBEAT_INTERVAL_MOVING_COMBAT
                                                                                                  : movementHandler_->getMoveHeartbeatInterval()));
        // The client's own mover sends its heartbeats on the original's schedule.
        if (!movementHandler_->clientMoverReports() &&
            movementHandler_->timeSinceLastMoveHeartbeatRef() >= heartbeatInterval) {
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
            movementHandler_->timeSinceLastMoveHeartbeatRef() = 0.0f;
        }

        // Check area triggers (instance portals, tavern rests, etc.)
        movementHandler_->updateAreaTriggers(deltaTime);

        // Cancel GO interaction cast if player enters combat (auto-attack).
        if (pendingGameObjectInteractGuid_ != 0 &&
            (combatHandler_->isAutoAttacking() || combatHandler_->hasAutoAttackIntent())) {
            pendingGameObjectInteractGuid_ = 0;
            spellHandler_->resetCastState();
            addUIError("Interrupted.");
            chatHandler_->addSystemChatMessage("Interrupted.");
        }
        // Check if client-side cast timer expired (tick-down is in SpellHandler::updateTimers).
        // Three paths depending on whether this is a GO interaction or craft-queue cast:
        if (spellHandler_->isCasting() && spellHandler_->getCastTimeRemaining() <= 0.0f) {
            if (pendingGameObjectInteractGuid_ != 0) {
                // GO interaction cast: do NOT call resetCastState() here. The server
                // sends SMSG_SPELL_GO when the cast completes server-side (~50-200ms
                // after the client timer expires due to float precision/frame timing).
                // handleSpellGo checks `wasInTimedCast = casting_ && spellId == currentCastSpellId_`
                // - if we clear those fields now, wasInTimedCast is false and the loot
                // path (CMSG_LOOT via lastInteractedGoGuid_) never fires.
                // Let the cast bar sit at 100% until SMSG_SPELL_GO arrives to clean up.
                pendingGameObjectInteractGuid_ = 0;
            } else if (spellHandler_->getCraftQueueRemaining() > 0 && craftCastGoGraceSec_ < 2.0f) {
                // Craft queue cast: SMSG_SPELL_GO is what decrements the queue and
                // re-casts the next item, and it races this client-side timer.
                // resetCastState() here would wipe the queue mid-run ("Create All"
                // stopping after one item), so let the cast bar sit at 100% briefly.
                // The 2s grace bails out if SPELL_GO never arrives (cast failed
                // without a result packet, e.g. reagents gone).
                craftCastGoGraceSec_ += deltaTime;
            } else {
                // Regular cast with no GO pending: clean up immediately.
                spellHandler_->resetCastState();
            }
        } else {
            craftCastGoGraceSec_ = 0.0f;
        }

        // Unit cast states and spell cooldowns are ticked by SpellHandler::updateTimers()
        // (called from GameHandler::updateTimers above), and the action bar asks
        // it for them - there is no copy on the buttons to tick.

        // Update combat text (Phase 2)
        combatHandler_->updateCombatText(deltaTime);
        socialHandler_->ageMinimapPings(deltaTime);
        playerHandler_->tickMirrorTimers(deltaTime);

        // Tick logout countdown
        socialHandler_->updateLogoutCountdown(deltaTime);

        updateTaxiAndMountState(deltaTime);

        // Update transport manager
        transportManager_->setRiderTransport(movementHandler_->getPlayerTransportGuid());
        transportManager_->update(deltaTime);
        movementHandler_->updateAttachedTransportChildren(deltaTime);

        combatHandler_->updateAutoAttack(deltaTime);
        auto closeIfTooFar = [&](bool windowOpen, uint64_t npcGuid, auto closeFn, const char* label) {
            if (!windowOpen || npcGuid == 0) return;
            auto npc = worldHandler_->getEntityManager().getEntity(npcGuid);
            if (!npc) return;
            float dx = movementInfo.x - npc->getX();
            float dy = movementInfo.y - npc->getY();
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > game::NPC_INTERACT_MAX_DISTANCE) {
                closeFn();
                // Warning: a window closing by itself is a report waiting to
                // happen, and the client is the one deciding.
                LOG_WARNING(label, " closed: ", distance, " yards from the NPC, past ",
                            game::NPC_INTERACT_MAX_DISTANCE);
            }
        };
        // A hello the server never answered. It says nothing when it refuses,
        // so this is the only place the refusal can be seen.
        if (pendingNpcHello_.guid != 0 &&
            std::chrono::steady_clock::now() - pendingNpcHello_.sentAt > std::chrono::milliseconds(1500)) {
            const int reaction = pendingNpcHello_.reaction;
            // Where the server has the player: the last movement packet sent.
            // Far from the NPC while the client stands beside it means the
            // server never heard the walk over.
            float sentDistance = -1.0f;
            glm::vec3 sent;
            auto npc = worldHandler_->getEntityManager().getEntity(pendingNpcHello_.guid);
            if (npc && movementHandler_->lastSentPosition(sent)) {
                sentDistance = glm::length(sent - glm::vec3(npc->getX(), npc->getY(), npc->getZ()));
            }
            LOG_WARNING("Talk: ", pendingNpcHello_.name.empty() ? "an NPC" : pendingNpcHello_.name,
                        " (guid 0x", std::hex, pendingNpcHello_.guid, std::dec, ")",
                        " did not answer. Asked from ", pendingNpcHello_.distance,
                        " yards here and ", sentDistance,
                        " from the last position sent to the server (3D); npc flags 0x",
                        std::hex, pendingNpcHello_.npcFlags, std::dec,
                        "; movement allowed=", movementHandler_->isServerMovementAllowed(),
                        " taxi=", movementHandler_->isOnTaxiFlight(),
                        " taxi mount=", movementHandler_->isTaxiMountActive(),
                        "; it regards you as ",
                        reaction >= 1 && reaction <= 8 ? kReputationStandings[reaction - 1].name : "?",
                        ". The server refuses in silence when the NPC is out of reach, busy, "
                        "or regards you as Unfriendly or worse");
            pendingNpcHello_.guid = 0;
        }
        closeIfTooFar(inventoryHandler_->isVendorWindowOpen(), inventoryHandler_->getVendorItems().vendorGuid, [this]{ closeVendor(); }, "Vendor");
        closeIfTooFar(questHandler_->isGossipWindowOpen(), questHandler_->getCurrentGossip().npcGuid, [this]{ closeGossip(); }, "Gossip");
        // The movement handler's guid, not a copy of it: the one this class
        // kept was never assigned, so this asked about guid 0, found no NPC and
        // returned - the only one of these four windows that stayed open when
        // the player walked away from it.
        closeIfTooFar(movementHandler_->isTaxiWindowOpen(),
                      movementHandler_->getTaxiNpcGuid(),
                      [this]{ movementHandler_->closeTaxi(); }, "Taxi window");
        closeIfTooFar(inventoryHandler_->isTrainerWindowOpen(), inventoryHandler_->getTrainerSpells().trainerGuid, [this]{ inventoryHandler_->closeTrainer(); }, "Trainer");

        updateEntityInterpolation(deltaTime);

    }
}

// ============================================================
// Single-player local combat
// ============================================================

void GameHandler::raiseUiError(const std::string& message) {
    // On screen and nowhere else, which is where the real client puts these.
    //
    // This wrote the same line to the chat log as well, on the grounds that
    // these messages had been chat-only before the on-screen line existed and
    // taking them out would lose them. In a fight it loses nothing and costs a
    // great deal: "Not ready", "Not enough rage" and "You can't do that right
    // now" arrive once per rejected keypress, so a few seconds of combat
    // scrolls the log past everything that mattered - the loot, the whispers,
    // the guild chat. UIErrorsFrame is the red line above the middle of the
    // screen and it is transient on purpose.
    //
    // Through addUIError rather than firing the event again here: it already
    // raises UI_ERROR_MESSAGE, which is what addons watch and what
    // UIErrorsFrame is registered for.
    addUIError(message);
}

// ============================================================
// Taxi / Flight Path Handlers
// ============================================================

// ============================================================
// Server Info Command Handlers
// ============================================================

uint32_t GameHandler::generateClientSeed() {
    // Generate cryptographically random seed
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(1, 0xFFFFFFFF);
    return dis(gen);
}

void GameHandler::setState(WorldState newState) {
    if (state != newState) {
        LOG_DEBUG("World state: ", static_cast<int>(state), " -> ", static_cast<int>(newState));
        state = newState;
        characterListWait_ = 0.0f;
        characterListRetried_ = false;
    }
}

void GameHandler::fail(const std::string& reason) {
    LOG_ERROR("World connection failed: ", reason);
    if (state == WorldState::IN_WORLD) {
        loseWorldConnection();
        return;
    }
    setState(WorldState::FAILED);

    if (onFailure) {
        onFailure(reason);
    }
}

// ============================================================
// Player Skills
// ============================================================

std::string GameHandler::getCharacterConfigDir() {
    return core::getConfigRoot() + "/characters";
}

void GameHandler::saveCharacterConfig() {
    const Character* ch = getActiveCharacter();
    if (!ch || ch->name.empty()) return;

    std::string dir = getCharacterConfigDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string path = dir + "/" + ch->name + ".cfg";
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save character config to ", path);
        return;
    }

    out << "character_guid=" << playerGuid << "\n";
    out << "gender=" << static_cast<int>(ch->gender) << "\n";
    // For male/female, derive from gender; only nonbinary has a meaningful separate choice
    bool saveUseFemaleModel = (ch->gender == Gender::NONBINARY) ? ch->useFemaleModel
                                                                 : (ch->gender == Gender::FEMALE);
    out << "use_female_model=" << (saveUseFemaleModel ? 1 : 0) << "\n";

    // The action bar and the macros are not here. The bar is the server's
    // (SMSG_ACTION_BUTTONS), and the macros are account data; a copy here
    // was a second answer that went on winning wherever the server's was
    // empty, and was never the one another client saw.

    // The quest log itself is not saved: the server sends it on every login,
    // so a stored copy could only ever be staler than what arrives. What is
    // saved below is the tracker selection, which is a client-side choice the
    // server knows nothing about.

    // Save tracked quest IDs so the quest tracker restores on login
    if (const auto& tracked = questHandler_->trackedQuests(); !tracked.empty()) {
        std::string ids;
        for (uint32_t qid : tracked) {
            if (!ids.empty()) ids += ',';
            ids += std::to_string(qid);
        }
        out << "tracked_quests=" << ids << "\n";
    }

    // Which reputation headers are closed. The server has no opinion about it,
    // so this file is the only place it can survive a logout.
    if (const auto& collapsed = reputation::orNoReputation(services_.reputation).getCollapsedFactions();
        !collapsed.empty()) {
        std::string ids;
        for (uint32_t fid : collapsed) {
            if (!ids.empty()) ids += ',';
            ids += std::to_string(fid);
        }
        out << "collapsed_factions=" << ids << "\n";
    }
    if (const auto& collapsedSkills = spellHandler_->collapsedSkillCategories(); !collapsedSkills.empty()) {
        std::string ids;
        for (uint32_t cid : collapsedSkills) {
            if (!ids.empty()) ids += ',';
            ids += std::to_string(cid);
        }
        out << "collapsed_skill_categories=" << ids << "\n";
    }

    LOG_INFO("Character config saved to ", path);
}

void GameHandler::loadCharacterConfig() {
    const Character* ch = getActiveCharacter();
    if (!ch || ch->name.empty()) return;

    std::string path = getCharacterConfigDir() + "/" + ch->name + ".cfg";
    std::ifstream in(path);
    if (!in.is_open()) return;

    uint64_t savedGuid = 0;
    // Macros an older build kept here, by the id it gave them.
    std::map<uint32_t, Macro> legacyMacros;
    int savedGender = -1;
    int savedUseFemaleModel = -1;
    // The quest log's; handed to it once the file is read.
    std::unordered_set<uint32_t> trackedQuests;
    std::unordered_set<uint32_t> collapsedFactions;
    std::unordered_set<uint32_t> collapsedSkillCategories;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "character_guid") {
            try { savedGuid = std::stoull(val); } catch (...) {}
        } else if (key == "gender") {
            try { savedGender = std::stoi(val); } catch (...) {}
        } else if (key == "use_female_model") {
            try { savedUseFemaleModel = std::stoi(val); } catch (...) {}
        } else if (key.rfind("macro_", 0) == 0) {
            // Parse macro_N_text
            size_t firstUnder = 6; // length of "macro_"
            size_t secondUnder = key.find('_', firstUnder);
            if (secondUnder == std::string::npos) continue;
            uint32_t macroId = 0;
            try { macroId = static_cast<uint32_t>(std::stoul(key.substr(firstUnder, secondUnder - firstUnder))); } catch (...) { continue; }
            if (key.substr(secondUnder + 1) == "text" && !val.empty()) {
                // Unescape \n and \\ sequences
                std::string unescaped;
                unescaped.reserve(val.size());
                for (size_t i = 0; i < val.size(); ++i) {
                    if (val[i] == '\\' && i + 1 < val.size()) {
                        if (val[i+1] == 'n')  { unescaped += '\n'; ++i; }
                        else if (val[i+1] == '\\') { unescaped += '\\'; ++i; }
                        else { unescaped += val[i]; }
                    } else {
                        unescaped += val[i];
                    }
                }
                legacyMacros[macroId].body = std::move(unescaped);
            } else if (key.substr(secondUnder + 1) == "name" && !val.empty()) {
                legacyMacros[macroId].name = val;
            } else if (key.substr(secondUnder + 1) == "icon" && !val.empty()) {
                legacyMacros[macroId].icon = val;
            }
        } else if ((key == "tracked_quests" || key == "collapsed_factions" ||
                    key == "collapsed_skill_categories") && !val.empty()) {
            // Parse a comma-separated id list into the matching selection.
            // map_visible_quests was a fourth of these and is no longer read:
            // the quests whose objectives the maps draw are the ones in the
            // log and the ones being tracked. An old file still carrying the
            // key falls through to the unknown-key branch.
            auto& destination =
                  key == "tracked_quests"    ? trackedQuests
                : key == "collapsed_factions" ? collapsedFactions
                                              : collapsedSkillCategories;
            destination.clear();
            size_t tqPos = 0;
            while (tqPos <= val.size()) {
                size_t comma = val.find(',', tqPos);
                std::string idStr = (comma != std::string::npos)
                    ? val.substr(tqPos, comma - tqPos) : val.substr(tqPos);
                try {
                    uint32_t qid = static_cast<uint32_t>(std::stoul(idStr));
                    if (qid != 0) destination.insert(qid);
                } catch (...) {}
                if (comma == std::string::npos) break;
                tqPos = comma + 1;
            }
        }
        // action_bar_N_type / action_bar_N_id from an older build are not
        // read: the bar is what SMSG_ACTION_BUTTONS says it is.
    }

    questHandler_->restoreTrackedQuests(std::move(trackedQuests));
    reputation::orNoReputation(services_.reputation).restoreCollapsedFactions(std::move(collapsedFactions));
    spellHandler_->restoreCollapsedSkillCategories(std::move(collapsedSkillCategories));

    // Validate guid matches current character
    if (savedGuid != 0 && savedGuid != playerGuid) {
        LOG_WARNING("Character config guid mismatch for ", ch->name, ", using defaults");
        return;
    }

    // Apply saved gender and body type (allows nonbinary to persist even though server only stores male/female)
    if (savedGender >= 0 && savedGender <= 2) {
        for (auto& character : characters) {
            if (character.guid == playerGuid) {
                character.gender = static_cast<Gender>(savedGender);
                if (character.gender == Gender::NONBINARY) {
                    // Only nonbinary characters have a meaningful body type choice
                    if (savedUseFemaleModel >= 0) {
                        character.useFemaleModel = (savedUseFemaleModel != 0);
                    }
                } else {
                    // Male/female always use the model matching their gender
                    character.useFemaleModel = (character.gender == Gender::FEMALE);
                }
                LOG_INFO("Applied saved gender: ", getGenderName(character.gender),
                         ", body type: ", (character.useFemaleModel ? "feminine" : "masculine"));
                break;
            }
        }
    }

    // An older build numbered the account's macros 1-36 and the character's
    // 37-54 and kept both here. They go to the account data, where the
    // character's carry the id bit the protocol gives them; the next save of
    // this file no longer writes them.
    if (!legacyMacros.empty()) {
        std::vector<Macro> carried;
        for (auto& [oldId, macro] : legacyMacros) {
            if (macro.body.empty()) continue;
            macro.id = oldId > static_cast<uint32_t>(kMaxAccountMacros)
                           ? kCharacterMacroIdBase + (oldId - kMaxAccountMacros)
                           : oldId;
            if (macro.name.empty()) macro.name = "Macro " + std::to_string(oldId);
            carried.push_back(std::move(macro));
        }
        LOG_INFO("Character config ", path, " holds ", carried.size(),
                 " macros from an older build; carrying them into the account data");
        accountDataHandler_->adoptLegacyMacros(std::move(carried));
    }
}





// ============================================================
// Bank System
// ============================================================

uint32_t GameHandler::getBankBagSlotPrice(int slotIndex) const {
    return InventoryHandler::getBankBagSlotPrice(slotIndex);
}

// ---------------------------------------------------------------------------
// SMSG_QUEST_CONFIRM_ACCEPT (shared quest from group member)
//   uint32 questId + string questTitle + uint64 sharerGuid
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SMSG_SUMMON_REQUEST
//   uint64 summonerGuid + uint32 zoneId + uint32 timeoutMs
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Group loot roll (SMSG_LOOT_ROLL / SMSG_LOOT_ROLL_WON / CMSG_LOOT_ROLL)
// ---------------------------------------------------------------------------

const GameHandler::BattlemasterEntry* GameHandler::getBattlemasterInfo(uint32_t bgTypeId) {
    if (!battlemasterListLoaded_) {
        battlemasterListLoaded_ = true;
        // Read at the WotLK file's columns, verified against it; an expansion
        // that does not list the table has no battleground list to offer.
        if (services_.gameData->has("BattlemasterList.dbc")) {
            const auto dbc = services_.gameData->table("BattlemasterList.dbc", 32,
                                                       "the battleground list");
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t id = dbc->getUInt32(i, 0);
                if (id == 0) continue;
                BattlemasterEntry e;
                e.id           = id;
                e.name         = dbc->getString(i, 11);
                e.maxGroupSize = dbc->getUInt32(i, 28);
                e.minLevel     = dbc->getUInt32(i, 30);
                e.maxLevel     = dbc->getUInt32(i, 31);
                // Fields 1-8 are map ids, 0xFFFFFFFF where unused, and field 9
                // is the instance type - verified against the file rather than
                // taken from a layout table, which does not carry this row.
                e.instanceType = dbc->getUInt32(i, 9);
                for (uint32_t f = 1; f <= 8; ++f) {
                    const uint32_t mapId = dbc->getUInt32(i, f);
                    if (mapId == 0xFFFFFFFFu) continue;
                    e.mapCount++;
                    e.mapIds.push_back(mapId);
                }
                if (e.instanceType == 3 && !e.name.empty()) battlegroundTypes_.push_back(e);
                battlemasterList_[id] = std::move(e);
            }
            // Ordered by id, so an index handed to the interface still means
            // the same battleground the next time it asks.
            std::sort(battlegroundTypes_.begin(), battlegroundTypes_.end(),
                      [](const BattlemasterEntry& a, const BattlemasterEntry& b) {
                          return a.id < b.id;
                      });
            LOG_INFO("Battleground: ", battlemasterList_.size(), " from BattlemasterList.dbc");
        }
    }
    auto it = battlemasterList_.find(bgTypeId);
    return it != battlemasterList_.end() ? &it->second : nullptr;
}

bool GameHandler::isBattlegroundMap(uint32_t mapId) {
    // Only the battleground rows, so an arena does not read as one. The pooled
    // "Random Battleground" row names the same maps the individual rows do, so
    // it costs nothing to walk and cannot answer wrongly.
    for (const auto& bg : getBattlegroundTypes()) {
        for (uint32_t id : bg.mapIds) {
            if (id == mapId) return true;
        }
    }
    return false;
}

bool GameHandler::isArenaMap(uint32_t mapId) {
    getBattlemasterInfo(0);   // latch the load, as isBattlegroundMap does
    for (const auto& [id, e] : battlemasterList_) {
        (void)id;
        if (e.instanceType != 4) continue;   // 4 = arena
        for (uint32_t m : e.mapIds) {
            if (m == mapId) return true;
        }
    }
    return false;
}

const std::vector<GameHandler::BattlemasterEntry>& GameHandler::getBattlegroundTypes() {
    // Through the same accessor, because that is where the load is latched.
    // Asking for the list before anything asked for an entry would otherwise
    // answer empty and latch nothing.
    getBattlemasterInfo(0);
    return battlegroundTypes_;
}

// CurrencyTypes.dbc: id, then the item that carries the amount. Everything the
// currency tab shows about a currency - name, icon, how many - comes from that
// item, so the row itself needs nothing else.
const std::vector<GameHandler::CurrencyType>& GameHandler::getCurrencyTypes() {
    if (currencyTypesLoaded_) return currencyTypes_;
    currencyTypesLoaded_ = true;
    // Currencies came with WotLK: before it there is no tab to fill.
    if (!services_.gameData->has("CurrencyTypes.dbc")) return currencyTypes_;

    const auto dbc = services_.gameData->table("CurrencyTypes.dbc", 2, "the currency tab");
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        CurrencyType c;
        c.id     = dbc->getUInt32(i, 0);
        c.itemId = dbc->getUInt32(i, 1);
        if (c.id == 0 || c.itemId == 0) continue;
        currencyTypes_.push_back(c);
    }
    LOG_INFO("Currency: ", currencyTypes_.size(), " types from CurrencyTypes.dbc");
    return currencyTypes_;
}

void GameHandler::ensureGlyphPropertiesLoaded() {
    if (glyphPropertiesLoaded_) return;
    glyphPropertiesLoaded_ = true;
    // Glyphs came with WotLK.
    if (!services_.gameData->has("GlyphProperties.dbc")) return;

    const auto dbc = services_.gameData->table("GlyphProperties.dbc", 2, "the glyphs' spells");
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const uint32_t id = dbc->getUInt32(i, 0);
        const uint32_t spellId = dbc->getUInt32(i, 1);
        if (id != 0 && spellId != 0) glyphSpellCache_[id] = spellId;
    }
    LOG_INFO("Glyphs: ", glyphSpellCache_.size(), " properties mapped to spells");
}

// ---------------------------------------------------------------------------
// Faction name cache (lazily loaded from Faction.dbc)
// ---------------------------------------------------------------------------

int GameHandler::unitReactionToPlayer(const Unit& unit) const {
    const uint32_t ft = unit.getFactionTemplate();
    if (auto it = factionTemplateRepList_.find(ft); it != factionTemplateRepList_.end()) {
        if (const auto* standing = reputation::orNoReputation(services_.reputation).getStandingAt(it->second)) {
            int rank = reputationStandingFor(standing->standing).id;
            if (standing->flags & reputation::kFactionAtWar) rank = std::min(rank, 4);
            return rank;
        }
    }
    if (unit.isHostile()) return 2;
    return isFriendlyFaction(ft) ? 5 : 4;
}

int GameHandler::factionAtWarLive(uint32_t factionTemplateId) const {
    auto it = factionTemplateRepList_.find(factionTemplateId);
    if (it == factionTemplateRepList_.end()) return -1;
    const auto* standing = reputation::orNoReputation(services_.reputation).getStandingAt(it->second);
    if (!standing) return -1;
    return (standing->flags & reputation::kFactionAtWar) != 0 ? 1 : 0;
}

void GameHandler::refreshUnitHostility() {
    if (factionTemplateRepList_.empty()) return;
    for (const auto& [guid, entity] : getEntityManager().getEntities()) {
        if (!entity || !entity->isUnit()) continue;
        auto* unit = static_cast<Unit*>(entity.get());
        unit->setHostile(isHostileFaction(unit->getFactionTemplate()));
    }
}

// ---------------------------------------------------------------------------
// Area name cache (lazy-loaded from WorldMapArea.dbc)
// ---------------------------------------------------------------------------

void GameHandler::loadAreaNameCache() const {
    if (areaNameCacheLoaded_) return;
    areaNameCacheLoaded_ = true;

    // AreaTable.dbc has the canonical zone/area names keyed by AreaID.
    // Field 0 = ID, field 11 = AreaName (enUS locale).
    {
        const auto areaDbc = services_.gameData->table("AreaTable.dbc", 12, "the names of zones and areas");
        for (uint32_t i = 0; i < areaDbc->getRecordCount(); ++i) {
            uint32_t areaId = areaDbc->getUInt32(i, 0);
            if (areaId == 0) continue;
            std::string name = areaDbc->getString(i, 11);
            if (!name.empty()) {
                areaNameCache_[areaId] = std::move(name);
            }
            // Field 4 is the flag word and field 28 the controlling team, which
            // together are the whole of what GetZonePVPInfo answers. Read while
            // the file is open rather than opening it twice.
            if (areaDbc->getFieldCount() > 28) {
                areaPvpCache_[areaId] = AreaPvpInfo{.flags = areaDbc->getUInt32(i, 4),
                                                    .team = areaDbc->getUInt32(i, 28)};
            }
        }
    }

    // WorldMapArea.dbc supplements with map-UI area names (different ID space).
    {
        const auto* layout = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("WorldMapArea") : nullptr;
        const uint32_t areaIdField   = layout ? (*layout)["AreaID"]   : 2;
        const uint32_t areaNameField = layout ? (*layout)["AreaName"] : 3;
        const auto dbc = services_.gameData->table("WorldMapArea.dbc",
                                                   std::max(areaIdField, areaNameField) + 1,
                                                   "the names of zones and areas");
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            uint32_t areaId = dbc->getUInt32(i, areaIdField);
            if (areaId == 0) continue;
            std::string name = dbc->getString(i, areaNameField);
            // Don't overwrite AreaTable names - those are authoritative
            if (!name.empty() && !areaNameCache_.count(areaId)) {
                areaNameCache_[areaId] = std::move(name);
            }
        }
    }

    LOG_INFO("Area name cache: loaded ", areaNameCache_.size(), " entries");
}

std::string GameHandler::getAreaName(uint32_t areaId) const {
    if (areaId == 0) return {};
    loadAreaNameCache();
    auto it = areaNameCache_.find(areaId);
    return (it != areaNameCache_.end()) ? it->second : std::string{};
}

void GameHandler::loadMapNameCache() const {
    if (mapNameCacheLoaded_) return;
    mapNameCacheLoaded_ = true;

    const auto dbc = services_.gameData->table("Map.dbc", 5,
                                               "the names of maps and whether one is an instance");

    // Map.dbc layout: 0=ID, 1=InternalName, 2=InstanceType, 3=Flags,
    // 4=MapName_enUS (display name), fields 5+ = other locales
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t id = dbc->getUInt32(i, 0);
        mapInstanceTypeCache_[id] = dbc->getUInt32(i, 2);
        std::string name = dbc->getString(i, 4);
        if (name.empty()) name = dbc->getString(i, 1); // internal name fallback
        if (!name.empty() && !mapNameCache_.count(id)) {
            mapNameCache_[id] = std::move(name);
        }
    }
    LOG_INFO("Map.dbc: loaded ", mapNameCache_.size(), " map names");
}

std::string GameHandler::getMapName(uint32_t mapId) const {
    loadMapNameCache();
    auto it = mapNameCache_.find(mapId);
    return (it != mapNameCache_.end()) ? it->second : std::string{};
}

void GameHandler::sendOptOutOfLoot(bool optOut) {
    if (!isInWorld() || !getSocket()) return;
    // One uint32, one or zero. The server keeps the state and never sends it
    // back, which is why the interface remembers its own copy.
    network::Packet packet(wireOpcode(Opcode::CMSG_OPT_OUT_OF_LOOT));
    packet.writeUInt32(optOut ? 1u : 0u);
    getSocket()->send(packet);
    LOG_INFO("CMSG_OPT_OUT_OF_LOOT: ", optOut ? "opting out" : "opting in");
}

void GameHandler::runInterfaceCommand(const std::string& lua) {
    if (!interfaceCommand_) {
        // A key reaching here before the interface is up does nothing, and does
        // it silently. Said out loud, because "the window did not open" is
        // otherwise indistinguishable from a key that never arrived.
        LOG_WARNING("interface command dropped, no interface yet: ", lua);
        return;
    }
    // At debug level: it is every key and every panel the client opens, and
    // some commands are a dozen lines of Lua. The two lines that matter at
    // warning are the ones either side - dropped for want of an interface, and
    // failed when it ran. WOWEE_LOG_LEVEL=debug separates "the key never
    // arrived" from "it arrived and the interface did nothing with it".
    LOG_DEBUG("interface command: ", lua);
    pendingAddonEvents_.command(lua);
}

const std::vector<ChannelMember>&
GameHandler::getChannelRoster(const std::string& channel) const {
    static const std::vector<ChannelMember> empty;
    return chatHandler_->getChannelRoster(channel);
}

bool GameHandler::getInstanceLockPrompt(float& secondsLeft, bool& previouslySaved,
                                        uint32_t& completedMask) const {
    const auto& prompt = socialHandler_->getInstanceLockPrompt();
    if (!prompt.active) return false;
    secondsLeft = prompt.secondsLeft;
    previouslySaved = prompt.previouslySaved;
    completedMask = prompt.completedEncounterMask;
    return true;
}

uint32_t GameHandler::getDungeonEncounterCount(uint32_t mapId, uint32_t difficulty) const {
    if (!dungeonEncounterCacheLoaded_) {
        dungeonEncounterCacheLoaded_ = true;
        // Encounters are counted from WotLK on; before it there are none.
        if (services_.gameData->has("DungeonEncounter.dbc")) {
            const auto dbc = services_.gameData->table("DungeonEncounter.dbc", 3,
                                                       "how many bosses an instance has");
            // ID, MapID, Difficulty, OrderIndex, Bit, Name[17], SpellIconID.
            // Verified against record 0: id 464, map 33 (Shadowfang Keep),
            // difficulty 0, order 0, bit 0.
            constexpr uint32_t kMapField = 1, kDiffField = 2;
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint64_t map = dbc->getUInt32(i, kMapField);
                const uint64_t diff = dbc->getUInt32(i, kDiffField);
                dungeonEncounterCounts_[(map << 32) | diff]++;
                // Most instances list their bosses once, under difficulty 0,
                // and apply to every difficulty. Keeping a per-map total means
                // a heroic run still has a number to show.
                dungeonEncounterCounts_[(map << 32) | 0xFFFFFFFFull]++;
            }
        }
        LOG_INFO("DungeonEncounter.dbc: ", dungeonEncounterCounts_.size(), " map/difficulty pairs");
    }

    auto it = dungeonEncounterCounts_.find((static_cast<uint64_t>(mapId) << 32) | difficulty);
    if (it != dungeonEncounterCounts_.end()) return it->second;
    it = dungeonEncounterCounts_.find((static_cast<uint64_t>(mapId) << 32) | 0xFFFFFFFFull);
    return (it != dungeonEncounterCounts_.end()) ? it->second : 0;
}

// ---------------------------------------------------------------------------
// LFG dungeon name cache (WotLK: LFGDungeons.dbc)
// ---------------------------------------------------------------------------

void GameHandler::loadLfgDungeonDbc() const {
    if (lfgDungeonNameCacheLoaded_) return;
    lfgDungeonNameCacheLoaded_ = true;
    // The dungeon finder is WotLK's.
    if (!services_.gameData->has("LFGDungeons.dbc")) return;

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("LFGDungeons") : nullptr;
    const uint32_t idField   = layout ? (*layout)["ID"]   : 0;
    const uint32_t nameField = layout ? (*layout)["Name"] : 1;

    // Everything past the name, which the layout table does not describe. The
    // WotLK file is 49 fields wide: sixteen name locales and their flags fill
    // 1-17, and the rest follow in this order. Read off the file and checked
    // against rows that can be recognised rather than taken on trust - Wailing
    // Caverns on map 43, Ragefire Chasm the one entry with faction 0, Karazhan
    // grouped with the Burning Crusade raids.
    constexpr uint32_t kMinLevel = 18, kMaxLevel = 19, kTargetLevel = 20;
    constexpr uint32_t kTargetMin = 21, kTargetMax = 22, kMapId = 23;
    constexpr uint32_t kDifficulty = 24, kTypeId = 26, kFaction = 27;
    constexpr uint32_t kTexture = 28, kExpansion = 29, kOrderIndex = 30, kGroupId = 31;
    // 32 through 48 are the description in each locale. Only the holiday rows
    // and a handful of others carry one.
    constexpr uint32_t kDescription = 32;
    // Group 11 is the four seasonal bosses - the Headless Horseman, Ahune,
    // Coren Direbrew and the Crown Chemical Co. - and nothing else. The file
    // has no holiday flag of its own; the group is the flag.
    constexpr uint32_t kHolidayGroup = 11;
    const auto dbc = services_.gameData->table(
        "LFGDungeons.dbc", std::max({idField, nameField, kGroupId}) + 1, "the dungeon finder");

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t id = dbc->getUInt32(i, idField);
        if (id == 0) continue;
        std::string name = dbc->getString(i, nameField);
        if (name.empty()) continue;
        lfgDungeonNameCache_[id] = name;

        LfgDungeon d;
        d.id          = id;
        d.name        = std::move(name);
        d.minLevel    = dbc->getUInt32(i, kMinLevel);
        d.maxLevel    = dbc->getUInt32(i, kMaxLevel);
        d.recLevel    = dbc->getUInt32(i, kTargetLevel);
        d.minRecLevel = dbc->getUInt32(i, kTargetMin);
        d.maxRecLevel = dbc->getUInt32(i, kTargetMax);
        d.mapId       = dbc->getUInt32(i, kMapId);
        d.difficulty  = dbc->getUInt32(i, kDifficulty);
        d.typeId      = dbc->getUInt32(i, kTypeId);
        d.faction     = static_cast<int32_t>(dbc->getUInt32(i, kFaction));
        d.texture     = dbc->getString(i, kTexture);
        d.expansion   = dbc->getUInt32(i, kExpansion);
        d.orderIndex  = dbc->getUInt32(i, kOrderIndex);
        d.groupId     = dbc->getUInt32(i, kGroupId);
        d.isHoliday   = (d.groupId == kHolidayGroup);
        if (dbc->getFieldCount() > kDescription) {
            d.description = dbc->getString(i, kDescription);
        }
        lfgDungeons_.push_back(std::move(d));
    }
    // The order the picker lists them in: by group, then by the level the
    // dungeon is meant for, so a category reads from lowest to highest.
    std::sort(lfgDungeons_.begin(), lfgDungeons_.end(),
              [](const LfgDungeon& a, const LfgDungeon& b) {
                  if (a.groupId != b.groupId) return a.groupId < b.groupId;
                  if (a.orderIndex != b.orderIndex) return a.orderIndex < b.orderIndex;
                  if (a.recLevel != b.recLevel) return a.recLevel < b.recLevel;
                  return a.id < b.id;
              });
    LOG_INFO("LFGDungeons.dbc: loaded ", lfgDungeonNameCache_.size(), " dungeon names, ",
             lfgDungeons_.size(), " listable entries");
}

// ---------------------------------------------------------------------------
// Mounts and critters
//
// The pet tab lists both, and both are spells the player knows. What separates
// them is what the spell does, which is in Spell.dbc:
//
//   a mount   applies aura 78 (SPELL_AURA_MOUNTED)
//   a critter uses effect 28 (SPELL_EFFECT_SUMMON) with EffectMiscValueB 41,
//             which is the critter SummonProperties row
//
// Both checked against the file rather than assumed: aura 78 picks out Brown
// Horse, Gray Wolf and White Stallion, and effect 28 with miscB 41 picks out
// Mechanical Squirrel, Cockroach and Worg Pup. EffectMiscValue beside it is the
// creature, which is what GetCompanionInfo reports first.
// ---------------------------------------------------------------------------
void GameHandler::rebuildCompanions() const {
    const auto& known = spellHandler_->getKnownSpells();
    // Only the spellbook changing can change the answer, and it changes rarely.
    if (companionsBuiltFromSpellCount_ == known.size()) return;
    companionsBuiltFromSpellCount_ = known.size();
    mountSpells_.clear();
    critterSpells_.clear();

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
    // Every column read below by the layout's name, or the WotLK file's
    // position where no layout is loaded (a test). A column an older layout
    // does not name - the second and third effects, EffectMiscValueB - reads
    // as zero, as it always has.
    constexpr uint32_t kNone = 0xFFFFFFFFu;
    const auto field = [layout](const std::string& name, uint32_t wotlk) {
        return layout ? layout->field(name) : wotlk;
    };
    const uint32_t idField = field("ID", 0);
    uint32_t widest = idField;
    std::array<uint32_t, 3> effect{}, aura{}, misc{}, miscB{};
    for (uint32_t i = 0; i < 3; ++i) {
        const std::string idx = std::to_string(i);
        effect[i] = field("Effect" + idx, 71 + i);
        aura[i]   = field("EffectApplyAuraName" + idx, 95 + i);
        misc[i]   = field("EffectMiscValue" + idx, 110 + i);
        miscB[i]  = field("EffectMiscValueB" + idx, 113 + i);
        for (uint32_t column : {effect[i], aura[i], misc[i], miscB[i]}) {
            if (column != kNone) widest = std::max(widest, column);
        }
    }
    const auto dbc = services_.gameData->table("Spell.dbc", widest + 1,
                                               "telling the player's mounts from their pets");
    const auto at = [&dbc](uint32_t row, uint32_t column) {
        return column == kNone ? 0u : dbc->getUInt32(row, column);
    };

    // Built once per rebuild rather than searched per spell: the file is fifty
    // thousand rows and the spellbook is a few hundred.
    std::unordered_map<uint32_t, uint32_t> rowOfSpell;
    rowOfSpell.reserve(dbc->getRecordCount());
    for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
        rowOfSpell[dbc->getUInt32(row, idField)] = row;
    }

    constexpr uint32_t kAuraMounted = 78;
    constexpr uint32_t kEffectSummon = 28;
    constexpr uint32_t kCritterSummonProperties = 41;

    for (uint32_t spellId : known) {
        auto it = rowOfSpell.find(spellId);
        if (it == rowOfSpell.end()) continue;
        const uint32_t row = it->second;

        Companion c;
        c.spellId = spellId;
        c.name = spellHandler_->getSpellName(spellId);
        if (c.name.empty()) continue;

        bool mount = false, critter = false;
        for (uint32_t i = 0; i < 3 && !mount && !critter; ++i) {
            if (at(row, aura[i]) == kAuraMounted) {
                mount = true;
                // SPELL_AURA_MOUNTED's misc value is a creature *entry*, the
                // same number space a summon's is. AuraEffect::HandleAuraMounted
                // opens with `uint32 creatureEntry = GetMiscValue();` and then
                // picks the model with ChooseDisplayId on that entry's
                // template - it is not handed to Player::Mount as a display id,
                // which is what the comment here used to say. Stored as the
                // entry and resolved by the reader, exactly like a critter's.
                c.creatureId = at(row, misc[i]);
            } else if (at(row, effect[i]) == kEffectSummon &&
                       at(row, miscB[i]) == kCritterSummonProperties) {
                critter = true;
                // A summon's misc value is a creature *entry*, not a display
                // id, and the model frame takes a display id. Left as the entry
                // and resolved in GetCompanionInfo, which is the only reader.
                // Both kinds are entries, so it no longer matters there which
                // kind was asked for.
                //
                // It used to resolve here when the cache happened to be warm
                // and leave the entry when it did not, on the reading that a
                // later rebuild would catch it - but nothing rebuilds on a
                // query answer and nothing sent the query, so the field meant a
                // display id or an entry depending on what had been near the
                // player. One number space, decided in one place.
                c.creatureId = at(row, misc[i]);
            }
        }
        if (!mount && !critter) continue;
        c.isMount = mount;
        (mount ? mountSpells_ : critterSpells_).push_back(std::move(c));
    }

    auto byName = [](const Companion& a, const Companion& b) { return a.name < b.name; };
    std::sort(mountSpells_.begin(), mountSpells_.end(), byName);
    std::sort(critterSpells_.begin(), critterSpells_.end(), byName);
    LOG_INFO("Companions: ", mountSpells_.size(), " mounts, ",
             critterSpells_.size(), " critters from ", known.size(), " known spells");
}

uint32_t GameHandler::getBonusActionBarOffset() const {
    const uint8_t form = playerHandler_->shapeshiftForm();
    if (form == 0) return 0;

    // Asked by GetBonusBarOffset when the bars change, not every frame, and
    // the asset manager keeps the file: the thirty rows are walked each time.
    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellShapeshiftForm") : nullptr;
    const uint32_t idField  = layout ? (*layout)["ID"] : 0;
    const uint32_t barField = layout ? (*layout)["BonusActionBar"] : 1;
    const auto dbc = services_.gameData->table("SpellShapeshiftForm.dbc",
                                               std::max(idField, barField) + 1,
                                               "the action bar a form swaps in");
    for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
        if (dbc->getUInt32(row, idField) == form) return dbc->getUInt32(row, barField);
    }
    // A form the file has no row for keeps the ordinary bar, as the original
    // client's lookup does; said once per form, since a missing row is news.
    static std::unordered_set<uint8_t> said;
    if (said.insert(form).second) {
        LOG_WARNING("SpellShapeshiftForm.dbc has no row for form ", static_cast<int>(form),
                    "; the action bar stays the ordinary one");
    }
    return 0;
}

void GameHandler::announceCompanionChange() {
    if (!hasAddonEventListener()) return;
    for (bool mounts : {true, false}) {
        uint32_t active = 0;
        for (const auto& c : getCompanions(mounts)) {
            for (const auto& a : spellHandler_->getPlayerAuras()) {
                if (a.spellId == c.spellId) { active = c.spellId; break; }
            }
            if (active) break;
        }
        uint32_t& remembered = mounts ? activeMountSpell_ : activeCritterSpell_;
        if (remembered == active) continue;
        remembered = active;
        fireAddonEvent("COMPANION_UPDATE", {mounts ? "MOUNT" : "CRITTER"});
    }
}

const std::vector<Companion>& GameHandler::getCompanions(bool mounts) const {
    rebuildCompanions();
    return mounts ? mountSpells_ : critterSpells_;
}
std::string GameHandler::companionKindForCreature(uint32_t entry) const {
    if (entry == 0) return {};
    // Both lists, not critters alone. A mount's creature id was taken for a
    // display id already, and it is not one: AzerothCore reads the mounted
    // aura's misc value with GetCreatureTemplate and picks the model with
    // ChooseDisplayId, so it is a template entry exactly as a critter's is.
    // Leaving mounts out meant a mount's model waited on a query that nothing
    // reported the answer to, and appeared on the next login instead.
    for (const Companion& c : getCompanions(/*mounts=*/false))
        if (c.creatureId == entry) return "CRITTER";
    for (const Companion& c : getCompanions(/*mounts=*/true))
        if (c.creatureId == entry) return "MOUNT";
    return {};
}

std::string GameHandler::getLfgDungeonName(uint32_t dungeonId) const {
    if (dungeonId == 0) return {};
    loadLfgDungeonDbc();
    auto it = lfgDungeonNameCache_.find(dungeonId);
    return (it != lfgDungeonNameCache_.end()) ? it->second : std::string{};
}

// ---- Battlefield Manager (WotLK Wintergrasp / outdoor battlefields) ----

// ---- WotLK Calendar ----

// ============================================================
// Delegating getters - SocialHandler owns the canonical state
// ============================================================

void GameHandler::resolveGMResponse() {
    if (getState() != WorldState::IN_WORLD || !getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GMRESPONSE_RESOLVE));
    getSocket()->send(packet);
}

void GameHandler::removeGlyphFromSocket(uint32_t slot) {
    if (slot >= MAX_GLYPH_SLOTS) return;
    if (getState() != WorldState::IN_WORLD || !getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_REMOVE_GLYPH));
    packet.writeUInt32(slot);
    getSocket()->send(packet);
}

bool GameHandler::isInInstance() const {
    // Difficulty packets advertise the player's preferred dungeon setting and
    // are also sent in the open world, so they cannot establish instance
    // presence. Map.dbc InstanceType is authoritative: 1=party, 2=raid.
    loadMapNameCache();
    auto it = mapInstanceTypeCache_.find(currentMapId_);
    return it != mapInstanceTypeCache_.end() &&
           (it->second == 1 || it->second == 2);
}

const std::string& GameHandler::getLanguageName(uint32_t languageId) const {
    static const std::string kUniversal = "Universal";
    static const std::string kNone;
    // Zero is not in the file: it is the "everyone understands this" language,
    // and it is also the name the interface tests for before deciding whether
    // to print a language header at all.
    if (languageId == 0) return kUniversal;
    if (!languageNamesLoaded_) {
        languageNamesLoaded_ = true;
        const auto dbc = services_.gameData->table("Languages.dbc", 2, "the chat's language names");
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            const uint32_t id = dbc->getUInt32(i, 0);
            std::string name = dbc->getString(i, 1);
            if (id != 0 && !name.empty()) languageNames_[id] = std::move(name);
        }
        LOG_INFO("Languages.dbc: loaded ", languageNames_.size(), " languages");
    }
    auto it = languageNames_.find(languageId);
    return it != languageNames_.end() ? it->second : kNone;
}

const std::string& GameHandler::getPageTextMaterialName(uint32_t materialId) const {
    static const std::string kNone;
    if (!pageTextMaterialsLoaded_) {
        pageTextMaterialsLoaded_ = true;
        const auto dbc = services_.gameData->table("PageTextMaterial.dbc", 2,
                                                   "the background of a book or letter");
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            const uint32_t id = dbc->getUInt32(i, 0);
            std::string name = dbc->getString(i, 1);
            if (id != 0 && !name.empty()) {
                pageTextMaterialNames_[id] = std::move(name);
            }
        }
        LOG_INFO("PageTextMaterial.dbc: loaded ",
                 pageTextMaterialNames_.size(), " page materials");
    }
    auto it = pageTextMaterialNames_.find(materialId);
    return it != pageTextMaterialNames_.end() ? it->second : kNone;
}

const GameHandler::ContinentBounds&
GameHandler::getContinentBounds(uint32_t mapId) const {
    auto cached = continentBoundsCache_.find(mapId);
    if (cached != continentBoundsCache_.end()) return cached->second;

    ContinentBounds bounds;
    {
        const auto* layout = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("WorldMapArea") : nullptr;
        const auto field = [&](const char* name, uint32_t fallback) {
            return layout ? (*layout)[name] : fallback;
        };
        const uint32_t mapField = field("MapID", 1), areaField = field("AreaID", 2);
        const uint32_t leftField = field("LocLeft", 4), rightField = field("LocRight", 5);
        const uint32_t topField = field("LocTop", 6), bottomField = field("LocBottom", 7);
        const auto dbc = services_.gameData->table(
            "WorldMapArea.dbc",
            std::max({mapField, areaField, leftField, rightField, topField, bottomField}) + 1,
            "a continent's map rectangle");
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            if (dbc->getUInt32(i, mapField) != mapId) continue;
            // The continent's own row, not one of its zones.
            if (dbc->getUInt32(i, areaField) != 0) continue;
            bounds.left   = dbc->getFloat(i, leftField);
            bounds.right  = dbc->getFloat(i, rightField);
            bounds.top    = dbc->getFloat(i, topField);
            bounds.bottom = dbc->getFloat(i, bottomField);
            // A degenerate rectangle projects everything to one point, so
            // it is no more usable than a missing row.
            bounds.valid = std::abs(bounds.left - bounds.right) > 0.001f &&
                           std::abs(bounds.top - bounds.bottom) > 0.001f;
            break;
        }
    }
    if (!bounds.valid) {
        LOG_WARNING("No continent-wide WorldMapArea row for map ", mapId,
                    " - anything projecting onto its map will have nothing to "
                    "project against");
    }
    return continentBoundsCache_.emplace(mapId, bounds).first->second;
}

const GameHandler::ExtendedCostEntry*
GameHandler::getExtendedCost(uint32_t extendedCostId) const {
    if (!extendedCostCacheLoaded_) {
        extendedCostCacheLoaded_ = true;
        // Prices in honor, arena points and tokens came with TBC.
        if (services_.gameData->has("ItemExtendedCost.dbc")) {
            const auto dbc = services_.gameData->table("ItemExtendedCost.dbc", 14,
                                                       "a vendor's prices in honor, arena points and tokens");
            // WotLK layout: 0=ID, 1=honorPoints, 2=arenaPoints,
            // 3=arenaSlotRestrictions, 4-8=itemId[5], 9-13=itemCount[5],
            // 14=reqRating, 15=purchaseGroup
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t id = dbc->getUInt32(i, 0);
                if (id == 0) continue;
                ExtendedCostEntry e;
                e.honorPoints = dbc->getUInt32(i, 1);
                e.arenaPoints = dbc->getUInt32(i, 2);
                for (int j = 0; j < 5; ++j) {
                    e.itemId[j]    = dbc->getUInt32(i, 4 + j);
                    e.itemCount[j] = dbc->getUInt32(i, 9 + j);
                }
                extendedCostCache_[id] = e;
            }
            LOG_INFO("ItemExtendedCost.dbc: loaded ", extendedCostCache_.size(),
                     " entries");
        }
    }
    auto it = extendedCostCache_.find(extendedCostId);
    return it == extendedCostCache_.end() ? nullptr : &it->second;
}

// ---- MovementHandler delegating getters ----

} // namespace game
} // namespace wowee
