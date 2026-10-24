#pragma once

#include <cstdint>
#include <string>

namespace wowee::arena {

/// Requests to the server about arena teams. Implemented by the protocol layer.
class IArenaGateway {
public:
    virtual ~IArenaGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    /// A connection, in the world or not.
    [[nodiscard]] virtual bool connected() const = 0;

    virtual void sendAccept() = 0;
    virtual void sendDecline() = 0;
    /// The team id and, for three of them, a player name - the shape
    /// ArenaTeamHandler reads: `recvData >> arenaTeamId >> name`.
    virtual void sendInvite(uint32_t teamId, const std::string& name) = 0;
    virtual void sendLeave(uint32_t teamId) = 0;
    virtual void sendRemove(uint32_t teamId, const std::string& name) = 0;
    virtual void sendLeader(uint32_t teamId, const std::string& name) = 0;
    virtual void sendDisband(uint32_t teamId) = 0;
    virtual void sendRosterRequest(uint32_t teamId) = 0;
};

} // namespace wowee::arena
