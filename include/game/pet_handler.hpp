#pragma once

#include "foundation/event_bus.hpp"
#include "game/opcode_table.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::network { class Packet; }
namespace wowee::pet { class IPetFeed; }

namespace wowee::game {

class PacketSender;
class FrameXmlEvents;
class ChatLog;

/// What the pet's protocol asks of the rest of the game.
class PetQueries {
public:
    virtual ~PetQueries() = default;

    [[nodiscard]] virtual std::string spellName(uint32_t spellId) const = 0;
    /// Give a unit in view the name its owner chose. False when it is not in
    /// view, or not a unit.
    virtual bool nameUnitInView(uint64_t guid, const std::string& name) = 0;
    /// The pet, or null in a client with no game.
    [[nodiscard]] virtual pet::IPetFeed* petFeed() = 0;
    /// The player's pet, or zero.
    [[nodiscard]] virtual uint64_t petGuid() const = 0;
};

/// The pet's and the stable's opcodes, decoded and handed to the pet context,
/// and what the player is told of them. Also asks what a summoned pet is
/// called, since the name a player gave it arrives in answer to that alone.
class PetHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    PetHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
               ChatLog& chat, PetQueries& queries);

    void registerOpcodes(DispatchTable& table);

    /// A character is being logged in: the last one's pet and stable are not
    /// this one's, and no name it asked for is still awaited.
    void beginCharacter();

private:
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);
    void handlePetNameQueryResponse(network::Packet& packet);
    void handleTameFailure(network::Packet& packet);
    void handleActionFeedback(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleMode(network::Packet& packet);

    /// Ask the server what the pet is called. The name a player gave a pet
    /// arrives only in answer to CMSG_PET_NAME_QUERY; without it the pet wears
    /// its creature template's name.
    void requestName(uint64_t petGuid);
    void raiseUiError(const std::string& message);
    /// The feed, or null with a warning said once.
    pet::IPetFeed* feed();

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    PetQueries& queries_;

    /// CMSG_PET_NAME_QUERY carries a key the server echoes back; this matches
    /// the reply to the pet it asked about.
    std::unordered_map<uint32_t, uint64_t> pendingNameQueries_;
    uint32_t nextNameQueryKey_ = 1;

    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace wowee::game
