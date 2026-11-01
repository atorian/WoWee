#pragma once

#include "trade/domain/records.hpp"

#include <cstdint>
#include <vector>

namespace wowee::trade {

/// A trade as the interface and the addon layer use it.
class ITrade {
public:
    virtual ~ITrade() = default;

    // ---- Reading ----
    [[nodiscard]] virtual bool isTradeOpen() const = 0;
    /// The player traded with: the unit FrameXML calls "NPC" while trading.
    [[nodiscard]] virtual uint64_t getTradePeerGuid() const = 0;
    [[nodiscard]] virtual const TradeSlots& getMyTradeSlots() const = 0;
    [[nodiscard]] virtual const TradeSlots& getPeerTradeSlots() const = 0;
    [[nodiscard]] virtual uint64_t getMyTradeGold() const = 0;
    [[nodiscard]] virtual uint64_t getPeerTradeGold() const = 0;

    // ---- Requests ----
    virtual void initiateTrade(uint64_t targetGuid) = 0;
    /// Answers another player's request (CMSG_BEGIN_TRADE).
    virtual void acceptTradeRequest() = 0;
    virtual void acceptTrade() = 0;
    /// Takes back an acceptance, leaving the trade open.
    virtual void unacceptTrade() = 0;
    virtual void cancelTrade() = 0;
    virtual void setTradeItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) = 0;
    virtual void clearTradeItem(uint8_t tradeSlot) = 0;
    virtual void setTradeGold(uint64_t copper) = 0;
};

/// `trade`, or the trade of a client with no game: never open, empty, and
/// every request dropped - for a reader that would otherwise test for it at
/// every call.
[[nodiscard]] ITrade& orNoTrade(ITrade* trade);

/// What the protocol layer tells the trade.
class ITradeFeed {
public:
    virtual ~ITradeFeed() = default;

    /// SMSG_TRADE_STATUS; the guid only comes with a request.
    virtual void onStatus(StatusCode code, uint64_t peerGuid) = 0;
    /// SMSG_TRADE_STATUS_EXTENDED: one side's offer.
    virtual void onOffer(bool ownSide, uint64_t gold, std::vector<OfferedSlot> slots) = 0;
    /// A character is logging in: none of the last one's trade is theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::trade
