#include "trade/domain/trade.hpp"

namespace wowee::trade {

std::vector<TradeChange> Trade::requested(uint64_t peerGuid, std::string peerName) {
    status_ = TradeStatus::PendingIncoming;
    peerGuid_ = peerGuid;
    peerName_ = std::move(peerName);
    return {TradeRequested{peerName_}};
}

std::vector<TradeChange> Trade::opened() {
    status_ = TradeStatus::Open;
    selfAccepted_ = partnerAccepted_ = false;
    return {TradeShown{}};
}

std::vector<TradeChange> Trade::partnerAccepted() {
    status_ = TradeStatus::Accepted;
    partnerAccepted_ = true;
    return {PartnerAccepted{}, acceptState()};
}

std::vector<TradeChange> Trade::backToTrade() {
    status_ = TradeStatus::Open;
    selfAccepted_ = partnerAccepted_ = false;
    return {acceptState()};
}

std::vector<TradeChange> Trade::ended(Ending how) {
    if (how == Ending::Completed) {
        // The final offer may still be on its way in the same batch, so what
        // was traded stays readable until the next trade.
        status_ = TradeStatus::None;
        selfAccepted_ = partnerAccepted_ = false;
    } else {
        reset();
    }
    return {TradeEnded{how}};
}

std::vector<TradeChange> Trade::refused(Refusal why) {
    switch (why) {
        // Conditions of the moment: the trade, if there is one, goes on.
        case Refusal::YouStunned:
        case Refusal::TargetStunned:
        case Refusal::YouDead:
        case Refusal::TargetDead:
        case Refusal::NotOnTapList:
            break;
        default:
            reset();
            break;
    }
    return {TradeRefused{why}};
}

std::vector<TradeChange> Trade::selfAccepted() {
    selfAccepted_ = true;
    return {acceptState()};
}

std::vector<TradeChange> Trade::selfUnaccepted() {
    selfAccepted_ = false;
    return {acceptState()};
}

std::vector<TradeChange> Trade::offerReceived(bool own, uint64_t gold, const std::vector<OfferedSlot>& slots) {
    auto& side = own ? mySlots_ : peerSlots_;
    for (const auto& s : slots) {
        if (s.index >= kSlotCount) continue;
        auto& slot = side[s.index];
        slot.itemId = s.itemId;
        slot.displayId = s.displayId;
        slot.stackCount = s.stackCount;
    }
    uint64_t& sideGold = own ? myGold_ : peerGold_;
    std::vector<TradeChange> changes{OffersChanged{}};
    if (sideGold != gold) changes.emplace_back(OfferedMoneyChanged{own});
    sideGold = gold;
    return changes;
}

} // namespace wowee::trade
