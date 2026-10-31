#include "game/query_cache_handler.hpp"

#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "game/entity.hpp"
#include "game/game_events.hpp"
#include "game/opcodes.hpp"
#include "game/packet_parsers.hpp"
#include "game/packet_sender.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee::game {

QueryCacheHandler::QueryCacheHandler(PacketSender& sender, core::EventBus& events,
                                     QueryCacheQueries& queries)
    : sender_(sender), events_(events), queries_(queries) {}

void QueryCacheHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_NAME_QUERY_RESPONSE] = [this](network::Packet& packet) {
        handleNameQueryResponse(packet);
    };
    table[Opcode::SMSG_CREATURE_QUERY_RESPONSE] = [this](network::Packet& packet) {
        handleCreatureQueryResponse(packet);
    };
    table[Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE] = [this](network::Packet& packet) {
        handleGameObjectQueryResponse(packet);
    };
}

void QueryCacheHandler::clear() {
    playerNames_.clear();
    playerClassRace_.clear();
    pendingNames_.clear();
    creatures_.clear();
    pendingCreatures_.clear();
    gameObjects_.clear();
    pendingGameObjects_.clear();
}

void QueryCacheHandler::queryPlayerName(uint64_t guid) {
    if (playerNames_.count(guid) || pendingNames_.count(guid)) return;
    if (!sender_.characterInWorld()) {
        LOG_INFO("queryPlayerName: skipped guid=0x", std::hex, guid, std::dec, " - not in the world");
        return;
    }
    LOG_INFO("queryPlayerName: sending CMSG_NAME_QUERY for guid=0x", std::hex, guid, std::dec);
    pendingNames_.insert(guid);
    sender_.send(NameQueryPacket::build(guid));
}

void QueryCacheHandler::queryCreatureInfo(uint32_t entry, uint64_t guid) {
    if (creatures_.count(entry) || pendingCreatures_.count(entry)) return;
    if (!sender_.characterInWorld()) return;
    pendingCreatures_.insert(entry);
    sender_.send(CreatureQueryPacket::build(entry, guid));
}

void QueryCacheHandler::queryGameObjectInfo(uint32_t entry, uint64_t guid) {
    if (gameObjects_.count(entry) || pendingGameObjects_.count(entry)) return;
    if (!sender_.characterInWorld()) return;
    pendingGameObjects_.insert(entry);
    sender_.send(GameObjectQueryPacket::build(entry, guid));
}

const std::string& QueryCacheHandler::lookupName(uint64_t guid) const {
    static const std::string kEmpty;
    auto it = playerNames_.find(guid);
    if (it != playerNames_.end()) return it->second;
    auto entity = queries_.entities().getEntity(guid);
    if (entity && entity->isUnit()) {
        // isUnit() is the type-tag check; skip dynamic_cast since the tag
        // already guarantees the Unit subclass.
        const auto* unit = static_cast<const Unit*>(entity.get());
        if (!unit->getName().empty()) return unit->getName();
    }
    return kEmpty;
}

void QueryCacheHandler::handleNameQueryResponse(network::Packet& packet) {
    NameQueryResponseData data;
    if (!queries_.packetParsers().parseNameQueryResponse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_NAME_QUERY_RESPONSE (size=", packet.getSize(), ")");
        return;
    }
    pendingNames_.erase(data.guid);

    LOG_INFO("Name query response: guid=0x", std::hex, data.guid, std::dec,
             " found=", static_cast<int>(data.found), " name='", data.name, "'",
             " race=", static_cast<int>(data.race), " class=", static_cast<int>(data.classId));
    if (!data.isValid()) return;

    playerNames_[data.guid] = data.name;
    // Class and race for the UnitClass/UnitRace fallback, and the gender an
    // NPC's $g switch chooses by.
    if (data.classId != 0 || data.race != 0) {
        playerClassRace_[data.guid] = {.classId = data.classId, .raceId = data.race,
                                       .gender = data.gender};
    }
    // After the cache above: chat releases the lines it held back for this
    // name, and a held NPC line reads the cache for its $-tokens. The mailbox
    // names a letter's sender, and the friend and ignore lists file the name.
    events_.publish(events::PlayerNameKnown{data.guid, data.name});
    queries_.playerNameArrived(data.guid, data.name);
}

void QueryCacheHandler::handleCreatureQueryResponse(network::Packet& packet) {
    CreatureQueryResponseData data;
    if (!queries_.packetParsers().parseCreatureQueryResponse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_CREATURE_QUERY_RESPONSE (size=", packet.getSize(),
                    "): the creature keeps no name and its query stays in flight");
        return;
    }
    pendingCreatures_.erase(data.entry);
    if (!data.isValid()) return;
    creatures_[data.entry] = data;
    queries_.creatureTemplateArrived(data);
}

void QueryCacheHandler::handleGameObjectQueryResponse(network::Packet& packet) {
    GameObjectQueryResponseData data;
    if (!queries_.packetParsers().parseGameObjectQueryResponse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_GAMEOBJECT_QUERY_RESPONSE (size=", packet.getSize(),
                    "): the object keeps no name and its query stays in flight");
        return;
    }
    pendingGameObjects_.erase(data.entry);
    if (!data.isValid()) return;
    gameObjects_[data.entry] = data;
    queries_.gameObjectTemplateArrived(data);
}

} // namespace wowee::game
