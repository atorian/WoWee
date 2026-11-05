#pragma once

#include "foundation/event_bus.hpp"
#include "trade/domain/records.hpp"
#include "trade/services/ports.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::core { class CVarStore; }
namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class InventoryHandler;
class InventoryQueries;
class ChatLog;
class FrameXmlEvents;

/// The trade service's ports, implemented over the world connection, the
/// name and item caches and the Block Trades setting.
class TradeGateway final : public trade::ITradeGateway, public trade::ITradeWorld {
public:
    TradeGateway(PacketSender& sender, InventoryHandler& inventory, InventoryQueries& world,
                 const core::CVarStore& cvars)
        : sender_(sender), inventory_(inventory), world_(world), cvars_(cvars) {}

    // ITradeGateway
    [[nodiscard]] bool online() const override;
    bool mayAskToTrade() override;
    void sendInitiate(uint64_t target) override;
    void sendBegin() override;
    void sendCancel() override;
    void sendAccept() override;
    void sendUnaccept() override;
    void sendSetItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) override;
    void sendClearItem(uint8_t tradeSlot) override;
    void sendSetGold(uint32_t copper) override;

    // ITradeWorld
    [[nodiscard]] std::string playerName(uint64_t guid) const override;
    [[nodiscard]] bool tradesBlocked() const override;
    void requestItem(uint32_t itemEntry) override;

private:
    PacketSender& sender_;
    InventoryHandler& inventory_;
    InventoryQueries& world_;
    const core::CVarStore& cvars_;
};

/// SMSG_TRADE_STATUS's code, or nothing for one this client does not know.
[[nodiscard]] std::optional<trade::StatusCode> tradeStatusCode(uint32_t status);

/// One side's offer as SMSG_TRADE_STATUS_EXTENDED carries it.
struct TradeOffer {
    bool ownSide = false;
    uint64_t gold = 0;
    std::vector<trade::OfferedSlot> slots;
};

/// The offer; nothing when it is too short to hold even its header.
[[nodiscard]] std::optional<TradeOffer> readTradeOffer(network::Packet& packet);

/// What the player sees of a trade: the TRADE_* events, the chat lines and
/// the interface errors, and the bags and coin redrawn after one completes.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeTradeNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                      InventoryHandler& inventory);

} // namespace wowee::game
