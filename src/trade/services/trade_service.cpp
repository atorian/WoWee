#include "trade/services/trade_service.hpp"

#include "foundation/event_bus.hpp"
#include "foundation/logger.hpp"

namespace wowee::trade {

namespace {

/// The trade of a client with no game: never open, empty, and every request
/// dropped.
class NoTrade final : public ITrade {
public:
    bool isTradeOpen() const override { return false; }
    uint64_t getTradePeerGuid() const override { return 0; }
    const TradeSlots& getMyTradeSlots() const override { return slots_; }
    const TradeSlots& getPeerTradeSlots() const override { return slots_; }
    uint64_t getMyTradeGold() const override { return 0; }
    uint64_t getPeerTradeGold() const override { return 0; }
    void initiateTrade(uint64_t) override {}
    void acceptTradeRequest() override {}
    void acceptTrade() override {}
    void unacceptTrade() override {}
    void cancelTrade() override {}
    void setTradeItem(uint8_t, uint8_t, uint8_t) override {}
    void clearTradeItem(uint8_t) override {}
    void setTradeGold(uint64_t) override {}

private:
    TradeSlots slots_{};
};

NoTrade kNoTrade;

} // namespace

ITrade& orNoTrade(ITrade* trade) {
    return trade ? *trade : kNoTrade;
}

TradeService::TradeService(ITradeGateway& gateway, ITradeWorld& world, core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

void TradeService::apply(const std::vector<TradeChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void TradeService::initiateTrade(uint64_t targetGuid) {
    if (!gateway_.mayAskToTrade()) {
        LOG_WARNING("Cannot initiate trade: not in world or not connected");
        return;
    }
    if (targetGuid == 0) {
        events_.publish(NoTradeTarget{});
        return;
    }
    gateway_.sendInitiate(targetGuid);
    events_.publish(TradeProposed{});
}

void TradeService::acceptTradeRequest() {
    if (trade_.isPending() && gateway_.online()) gateway_.sendBegin();
}

void TradeService::acceptTrade() {
    // Open or accepted: the status turns to accepted the moment the partner
    // presses accept, and whoever presses second must still be able to.
    if (!trade_.isOpen() || !gateway_.online()) return;
    gateway_.sendAccept();
    apply(trade_.selfAccepted());
}

void TradeService::unacceptTrade() {
    // Only an acceptance of ours can be taken back; the status turns to
    // accepted when the partner accepts, which is not one.
    if (!trade_.hasSelfAccepted() || !gateway_.online()) return;
    gateway_.sendUnaccept();
    apply(trade_.selfUnaccepted());
}

void TradeService::cancelTrade() {
    if (trade_.status() == TradeStatus::None) return;
    if (gateway_.online()) gateway_.sendCancel();
    trade_.reset();
}

void TradeService::setTradeItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) {
    if (trade_.isEditable() && gateway_.online()) gateway_.sendSetItem(tradeSlot, bag, bagSlot);
}

void TradeService::clearTradeItem(uint8_t tradeSlot) {
    if (trade_.isEditable() && gateway_.online()) gateway_.sendClearItem(tradeSlot);
}

void TradeService::setTradeGold(uint64_t copper) {
    if (trade_.isEditable() && gateway_.online()) gateway_.sendSetGold(static_cast<uint32_t>(copper));
}

void TradeService::onRequest(uint64_t peerGuid) {
    std::string name = world_.playerName(peerGuid);
    if (name.empty()) name = "Unknown";
    // Refused before the window rather than after: the panel offers Block
    // Trades, and every request used to open a window whatever it said.
    if (world_.tradesBlocked()) {
        events_.publish(TradeRequestBlocked{name});
        if (gateway_.online()) gateway_.sendCancel();
        trade_.reset();
        return;
    }
    apply(trade_.requested(peerGuid, std::move(name)));
}

void TradeService::onStatus(StatusCode code, uint64_t peerGuid) {
    switch (code) {
        case StatusCode::BeginTrade:    return onRequest(peerGuid);
        case StatusCode::OpenWindow:    return apply(trade_.opened());
        case StatusCode::Accepted:      return apply(trade_.partnerAccepted());
        case StatusCode::BackToTrade:   return apply(trade_.backToTrade());
        case StatusCode::Canceled:      return apply(trade_.ended(Ending::Cancelled));
        case StatusCode::Rejected:      return apply(trade_.ended(Ending::Declined));
        case StatusCode::CloseWindow:   return apply(trade_.ended(Ending::WindowClosed));
        case StatusCode::Complete:      return apply(trade_.ended(Ending::Completed));
        case StatusCode::Busy:
        case StatusCode::Busy2:         return apply(trade_.refused(Refusal::Busy));
        case StatusCode::NoTarget:      return apply(trade_.refused(Refusal::NoTarget));
        case StatusCode::TooFar:        return apply(trade_.refused(Refusal::TooFar));
        case StatusCode::WrongFaction:  return apply(trade_.refused(Refusal::WrongFaction));
        case StatusCode::IgnoreYou:     return apply(trade_.refused(Refusal::Ignored));
        case StatusCode::YouStunned:    return apply(trade_.refused(Refusal::YouStunned));
        case StatusCode::TargetStunned: return apply(trade_.refused(Refusal::TargetStunned));
        case StatusCode::YouDead:       return apply(trade_.refused(Refusal::YouDead));
        case StatusCode::TargetDead:    return apply(trade_.refused(Refusal::TargetDead));
        case StatusCode::YouLogout:
        case StatusCode::TargetLogout:  return apply(trade_.refused(Refusal::LoggingOut));
        case StatusCode::NotOnTapList:  return apply(trade_.refused(Refusal::NotOnTapList));
    }
}

void TradeService::onOffer(bool ownSide, uint64_t gold, std::vector<OfferedSlot> slots) {
    for (const auto& s : slots)
        if (s.itemId != 0) world_.requestItem(s.itemId);
    apply(trade_.offerReceived(ownSide, gold, slots));
}

} // namespace wowee::trade
