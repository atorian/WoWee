#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::lfg {

/// Requests to the server about the dungeon finder. Implemented by the
/// protocol layer, which sends nothing on a realm with no dungeon finder.
class ILfgGateway {
public:
    virtual ~ILfgGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    /// A connection, in the world or not.
    [[nodiscard]] virtual bool connected() const = 0;

    virtual void sendJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) = 0;
    virtual void sendLeave() = 0;
    virtual void sendSetRoles(uint8_t roles) = 0;
    virtual void sendProposalAnswer(uint32_t proposalId, bool accept) = 0;
    virtual void sendTeleport(bool toLfgDungeon) = 0;
    virtual void sendBootVote(bool vote) = 0;
    virtual void sendPlayerLockInfoRequest() = 0;
    virtual void sendPartyLockInfoRequest() = 0;
};

/// What the dungeon finder needs from the rest of the game.
class ILfgWorld {
public:
    virtual ~ILfgWorld() = default;

    /// The dungeon's name from LFGDungeons.dbc, empty when unknown.
    [[nodiscard]] virtual std::string dungeonName(uint32_t dungeonId) const = 0;
    /// The name, empty while it is not known.
    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    virtual void requestPlayerName(uint64_t guid) = 0;
    /// The name of a unit in view, if there is one by that guid.
    [[nodiscard]] virtual std::optional<std::string> unitName(uint64_t guid) const = 0;
};

} // namespace wowee::lfg
