#pragma once

#include "trade/domain/trade.hpp"
#include "trade/services/ports.hpp"
#include "trade/services/trade.hpp"


namespace wowee::core { class EventBus; }

namespace wowee::trade {

class TradeService final : public ITrade, public ITradeFeed {
public:
    TradeService(ITradeGateway& gateway, ITradeWorld& world, core::EventBus& events);

    // ITrade: reading
    [[nodiscard]] bool isTradeOpen() const override { return trade_.isOpen(); }
    [[nodiscard]] uint64_t getTradePeerGuid() const override { return trade_.peerGuid(); }
    [[nodiscard]] const TradeSlots& getMyTradeSlots() const override { return trade_.mySlots(); }
    [[nodiscard]] const TradeSlots& getPeerTradeSlots() const override { return trade_.peerSlots(); }
    [[nodiscard]] uint64_t getMyTradeGold() const override { return trade_.myGold(); }
    [[nodiscard]] uint64_t getPeerTradeGold() const override { return trade_.peerGold(); }

    // ITrade: requests
    void initiateTrade(uint64_t targetGuid) override;
    void acceptTradeRequest() override;
    void acceptTrade() override;
    void unacceptTrade() override;
    void cancelTrade() override;
    void setTradeItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) override;
    void clearTradeItem(uint8_t tradeSlot) override;
    void setTradeGold(uint64_t copper) override;

    // ITradeFeed
    void onStatus(StatusCode code, uint64_t peerGuid) override;
    void onOffer(bool ownSide, uint64_t gold, std::vector<OfferedSlot> slots) override;
    void beginCharacter() override { trade_.beginCharacter(); }

private:
    void onRequest(uint64_t peerGuid);
    void apply(const std::vector<TradeChange>& changes);

    ITradeGateway& gateway_;
    ITradeWorld& world_;
    core::EventBus& events_;
    Trade trade_;
};

} // namespace wowee::trade
