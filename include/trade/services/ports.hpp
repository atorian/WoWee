#pragma once

#include <cstdint>
#include <string>

namespace wowee::trade {

/// Requests to the server about a trade. Implemented by the protocol layer.
class ITradeGateway {
public:
    virtual ~ITradeGateway() = default;

    /// In the world, with a connection.
    [[nodiscard]] virtual bool online() const = 0;
    /// Whether a trade may be asked for now. With the world connection lost
    /// and being restored, the player is told so.
    virtual bool mayAskToTrade() = 0;

    virtual void sendInitiate(uint64_t target) = 0;
    virtual void sendBegin() = 0;
    virtual void sendCancel() = 0;
    virtual void sendAccept() = 0;
    virtual void sendUnaccept() = 0;
    virtual void sendSetItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) = 0;
    virtual void sendClearItem(uint8_t tradeSlot) = 0;
    virtual void sendSetGold(uint32_t copper) = 0;
};

/// What a trade needs from the rest of the game.
class ITradeWorld {
public:
    virtual ~ITradeWorld() = default;

    /// The name, empty while it is not known.
    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    /// The Block Trades setting.
    [[nodiscard]] virtual bool tradesBlocked() const = 0;
    virtual void requestItem(uint32_t itemEntry) = 0;
};

} // namespace wowee::trade
