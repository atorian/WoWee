#pragma once

#include "game/opcode_table.hpp"
#include "game/world_packet_data.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wowee::core { class EventBus; }
namespace wowee::network { class Packet; }

namespace wowee::game {

class EntityManager;
class PacketParsers;
class PacketSender;

/// What the query caches ask of the rest of the game, and whom they tell when
/// an answer arrives. The answers are handed on in this order, one packet at
/// a time, so the interface hears of each in the order it always has: the
/// objects in view first, then what the quests and transports make of it.
class QueryCacheQueries {
public:
    virtual ~QueryCacheQueries() = default;
    [[nodiscard]] virtual PacketParsers& packetParsers() = 0;
    /// For a name not asked for: a unit in view may carry one.
    [[nodiscard]] virtual const EntityManager& entities() const = 0;
    /// A player's name arrived, after PlayerNameKnown was published.
    virtual void playerNameArrived(uint64_t guid, const std::string& name) = 0;
    virtual void creatureTemplateArrived(const CreatureQueryResponseData& data) = 0;
    virtual void gameObjectTemplateArrived(const GameObjectQueryResponseData& data) = 0;
};

/// What the realm has said a guid or an entry is: players' names, races and
/// classes, and the creature and game object templates. The original client
/// keeps the same three caches. Each is asked for once - a second ask while
/// the first is in flight is dropped - and kept until the character changes.
class QueryCacheHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    QueryCacheHandler(PacketSender& sender, core::EventBus& events, QueryCacheQueries& queries);

    void registerOpcodes(DispatchTable& table);
    /// Forget everything: a character change or a disconnect.
    void clear();

    // --- Asking ---
    void queryPlayerName(uint64_t guid);
    void queryCreatureInfo(uint32_t entry, uint64_t guid);
    void queryGameObjectInfo(uint32_t entry, uint64_t guid);
    /// The name changed (a rename the server announced): ask again next time.
    void invalidatePlayerName(uint64_t guid) { playerNames_.erase(guid); }
    /// A player left view. A name asked for and not yet answered is asked
    /// again when they come back.
    void playerLeftView(uint64_t guid) { pendingNames_.erase(guid); }

    // --- Players ---
    /// The player's name as the realm said it, or a unit's in view; empty
    /// when neither knows.
    [[nodiscard]] const std::string& lookupName(uint64_t guid) const;
    [[nodiscard]] uint8_t lookupPlayerClass(uint64_t guid) const {
        auto it = playerClassRace_.find(guid);
        return it != playerClassRace_.end() ? it->second.classId : 0;
    }
    [[nodiscard]] uint8_t lookupPlayerRace(uint64_t guid) const {
        auto it = playerClassRace_.find(guid);
        return it != playerClassRace_.end() ? it->second.raceId : 0;
    }
    /// 0 male, 1 female, as the name query answers; 0xFF when not known.
    [[nodiscard]] uint8_t lookupPlayerGender(uint64_t guid) const {
        auto it = playerClassRace_.find(guid);
        return it != playerClassRace_.end() ? it->second.gender : 0xFF;
    }
    [[nodiscard]] const std::unordered_map<uint64_t, std::string>& getPlayerNameCache() const {
        return playerNames_;
    }

    // --- Creatures ---
    [[nodiscard]] const std::unordered_map<uint32_t, CreatureQueryResponseData>& getCreatureInfoCache() const {
        return creatures_;
    }
    [[nodiscard]] std::string getCachedCreatureName(uint32_t entry) const {
        auto it = creatures_.find(entry);
        return it != creatures_.end() ? it->second.name : "";
    }
    [[nodiscard]] std::string getCachedCreatureSubName(uint32_t entry) const {
        auto it = creatures_.find(entry);
        return it != creatures_.end() ? it->second.subName : "";
    }
    /// -1 while the template is not known.
    [[nodiscard]] int getCreatureRank(uint32_t entry) const {
        auto it = creatures_.find(entry);
        return it != creatures_.end() ? static_cast<int>(it->second.rank) : -1;
    }
    [[nodiscard]] uint32_t getCreatureType(uint32_t entry) const {
        auto it = creatures_.find(entry);
        return it != creatures_.end() ? it->second.creatureType : 0;
    }
    [[nodiscard]] uint32_t getCreatureFamily(uint32_t entry) const {
        auto it = creatures_.find(entry);
        return it != creatures_.end() ? it->second.family : 0;
    }

    // --- Game objects ---
    [[nodiscard]] const GameObjectQueryResponseData* getCachedGameObjectInfo(uint32_t entry) const {
        auto it = gameObjects_.find(entry);
        return it != gameObjects_.end() ? &it->second : nullptr;
    }

private:
    void handleNameQueryResponse(network::Packet& packet);
    void handleCreatureQueryResponse(network::Packet& packet);
    void handleGameObjectQueryResponse(network::Packet& packet);

    PacketSender& sender_;
    core::EventBus& events_;
    QueryCacheQueries& queries_;

    std::unordered_map<uint64_t, std::string> playerNames_;
    struct PlayerClassRace { uint8_t classId = 0; uint8_t raceId = 0; uint8_t gender = 0xFF; };
    std::unordered_map<uint64_t, PlayerClassRace> playerClassRace_;
    std::unordered_set<uint64_t> pendingNames_;
    std::unordered_map<uint32_t, CreatureQueryResponseData> creatures_;
    std::unordered_set<uint32_t> pendingCreatures_;
    std::unordered_map<uint32_t, GameObjectQueryResponseData> gameObjects_;
    std::unordered_set<uint32_t> pendingGameObjects_;
};

} // namespace wowee::game
