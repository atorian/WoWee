#pragma once

#include "auction/services/ports.hpp"
#include "foundation/event_bus.hpp"

#include <vector>

namespace wowee::game {

class PacketSender;
class InventoryHandler;
class InventoryQueries;
class ChatLog;
class FrameXmlEvents;

/// The auction service's ports, implemented over the world connection, the
/// backpack and the item and name caches.
class AuctionGateway final : public auction::IAuctionGateway, public auction::IAuctionWorld {
public:
    AuctionGateway(PacketSender& sender, InventoryHandler& inventory, InventoryQueries& world)
        : sender_(sender), inventory_(inventory), world_(world) {}

    // IAuctionGateway
    [[nodiscard]] bool canSend() const override;
    void sendSearch(uint64_t auctioneer, const auction::SearchQuery& query) override;
    void sendSell(uint64_t auctioneer, uint64_t itemGuid, uint32_t stackCount,
                  uint32_t bid, uint32_t buyout, uint32_t duration) override;
    void sendBid(uint64_t auctioneer, uint32_t auctionId, uint32_t amount) override;
    void sendCancel(uint64_t auctioneer, uint32_t auctionId) override;
    void sendListOwned(uint64_t auctioneer, uint32_t offset) override;
    void sendListBids(uint64_t auctioneer, uint32_t offset) override;

    // IAuctionWorld
    [[nodiscard]] std::optional<BackpackItem> backpackItem(int index) const override;
    void prepareToShow(const auction::Entry& entry) override;

private:
    PacketSender& sender_;
    InventoryHandler& inventory_;
    InventoryQueries& world_;
};

/// What the player sees of the auction house: the panel's AUCTION_* events,
/// the chat lines and refusals for a command's answer, and the bags and coin
/// redrawn after one went through.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeAuctionNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                        InventoryHandler& inventory);

} // namespace wowee::game
