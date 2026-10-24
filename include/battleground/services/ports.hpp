#pragma once

#include <cstdint>
#include <string>

namespace wowee::battleground {

/// Requests to the server about battlegrounds. Implemented by the protocol
/// layer.
class IBattlegroundGateway {
public:
    virtual ~IBattlegroundGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;

    /// CMSG_BATTLEFIELD_PORT: go in, or turn the invitation or queue down.
    virtual void sendPort(uint8_t arenaType, uint32_t bgTypeId, bool accept) = 0;
    virtual void sendJoin(uint64_t battlemaster, uint32_t bgTypeId, uint32_t instanceId, bool asGroup) = 0;
    virtual void sendListRequest(uint32_t bgTypeId) = 0;
    virtual void sendReportAfk(uint64_t playerGuid) = 0;
    virtual void sendLeave() = 0;
    virtual void sendScoreRequest() = 0;
    virtual void sendPositionsRequest() = 0;
};

/// What battlegrounds need from the rest of the game.
class IBattlegroundWorld {
public:
    virtual ~IBattlegroundWorld() = default;

    /// The name, empty while it is not known.
    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    /// Whether the player is on a battleground map. The server drops a
    /// position request from anywhere else.
    [[nodiscard]] virtual bool inBattleground() const = 0;
};

} // namespace wowee::battleground
