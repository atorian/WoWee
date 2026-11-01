#pragma once

#include "trade/domain/events.hpp"
#include "trade/domain/records.hpp"

#include <string>
#include <vector>

namespace wowee::trade {

enum class TradeStatus : uint8_t { None, PendingIncoming, Open, Accepted };

/// A trade with another player: who, how far it has got, what each side has
/// put down, and which side has accepted.
class Trade {
public:
    std::vector<TradeChange> requested(uint64_t peerGuid, std::string peerName);
    std::vector<TradeChange> opened();
    std::vector<TradeChange> partnerAccepted();
    /// An acceptance was taken back; both start over.
    std::vector<TradeChange> backToTrade();
    std::vector<TradeChange> ended(Ending how);
    std::vector<TradeChange> refused(Refusal why);
    std::vector<TradeChange> selfAccepted();
    std::vector<TradeChange> selfUnaccepted();
    std::vector<TradeChange> offerReceived(bool own, uint64_t gold, const std::vector<OfferedSlot>& slots);
    void reset() { *this = Trade{}; }
    /// A character is logging in: no trade, offer or partner is theirs.
    void beginCharacter() { reset(); }

    [[nodiscard]] TradeStatus status() const { return status_; }
    [[nodiscard]] bool isPending() const { return status_ == TradeStatus::PendingIncoming; }
    /// Open for changes: neither side has accepted.
    [[nodiscard]] bool isEditable() const { return status_ == TradeStatus::Open; }
    [[nodiscard]] bool isOpen() const { return status_ == TradeStatus::Open || status_ == TradeStatus::Accepted; }
    [[nodiscard]] bool hasSelfAccepted() const { return selfAccepted_; }
    [[nodiscard]] uint64_t peerGuid() const { return peerGuid_; }
    [[nodiscard]] const std::string& peerName() const { return peerName_; }
    [[nodiscard]] const TradeSlots& mySlots() const { return mySlots_; }
    [[nodiscard]] const TradeSlots& peerSlots() const { return peerSlots_; }
    [[nodiscard]] uint64_t myGold() const { return myGold_; }
    [[nodiscard]] uint64_t peerGold() const { return peerGold_; }

private:
    [[nodiscard]] AcceptStateChanged acceptState() const { return {selfAccepted_, partnerAccepted_}; }

    TradeStatus status_ = TradeStatus::None;
    /// Kept apart because TRADE_ACCEPT_UPDATE carries both and the status can
    /// say only one thing at a time. Ours is set as the accept goes out: the
    /// server tells the other player, not the one who accepted.
    bool selfAccepted_ = false;
    bool partnerAccepted_ = false;
    uint64_t peerGuid_ = 0;
    std::string peerName_;
    TradeSlots mySlots_{};
    TradeSlots peerSlots_{};
    uint64_t myGold_ = 0;
    uint64_t peerGold_ = 0;
};

} // namespace wowee::trade
