#include "game/auction_gateway.hpp"

#include "auction/domain/events.hpp"
#include "foundation/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_utils.hpp"
#include "game/inventory_handler.hpp"
#include "game/packet_sender.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"

namespace wowee::game {

bool AuctionGateway::canSend() const {
    return sender_.characterInWorld();
}

void AuctionGateway::sendSearch(uint64_t auctioneer, const auction::SearchQuery& q) {
    sender_.send(AuctionListItemsPacket::build(auctioneer, q.offset, q.name, q.levelMin, q.levelMax,
                                               q.invTypeMask, q.itemClass, q.itemSubClass, q.quality,
                                               q.usableOnly, 0, q.sort));
}

void AuctionGateway::sendSell(uint64_t auctioneer, uint64_t itemGuid, uint32_t stackCount,
                              uint32_t bid, uint32_t buyout, uint32_t duration) {
    sender_.send(AuctionSellItemPacket::build(auctioneer, itemGuid, stackCount, bid, buyout, duration,
                                              isPreWotlk()));
}

void AuctionGateway::sendBid(uint64_t auctioneer, uint32_t auctionId, uint32_t amount) {
    sender_.send(AuctionPlaceBidPacket::build(auctioneer, auctionId, amount));
}

void AuctionGateway::sendCancel(uint64_t auctioneer, uint32_t auctionId) {
    sender_.send(AuctionRemoveItemPacket::build(auctioneer, auctionId));
}

void AuctionGateway::sendListOwned(uint64_t auctioneer, uint32_t offset) {
    sender_.send(AuctionListOwnerItemsPacket::build(auctioneer, offset));
}

void AuctionGateway::sendListBids(uint64_t auctioneer, uint32_t offset) {
    sender_.send(AuctionListBidderItemsPacket::build(auctioneer, offset));
}

std::optional<auction::IAuctionWorld::BackpackItem> AuctionGateway::backpackItem(int index) const {
    const auto& items = inventory_.items();
    if (index < 0 || index >= items.getBackpackSize()) return std::nullopt;
    const auto& slot = items.getBackpackSlot(index);
    if (slot.empty()) return std::nullopt;
    uint64_t guid = inventory_.getBackpackItemGuid(index);
    if (guid == 0) guid = inventory_.resolveOnlineItemGuid(slot.item.itemId);
    if (guid == 0) {
        LOG_ERROR("auctionSellItem: could not resolve GUID for backpack slot ", index);
        return std::nullopt;
    }
    return BackpackItem{guid, slot.item.stackCount};
}

void AuctionGateway::prepareToShow(const auction::Entry& entry) {
    inventory_.ensureItemInfo(entry.itemEntry);
    if (entry.ownerGuid != 0) world_.requestPlayerName(entry.ownerGuid);
}

std::vector<core::EventBus::Subscription>
subscribeAuctionNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                        InventoryHandler& inventory) {
    using namespace auction;
    std::vector<core::EventBus::Subscription> s;
    auto addon = [&frameXml](const char* name) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, {});
    };
    auto actionName = [](uint32_t action) {
        static const char* kNames[] = {"sell", "cancel", "bid/buyout"};
        return action < 3 ? kNames[action] : "unknown";
    };

    s.push_back(events.subscribe<AuctionHouseOpened>([addon](const auto&) { addon("AUCTION_HOUSE_SHOW"); }));
    s.push_back(events.subscribe<AuctionHouseClosed>([addon](const auto&) { addon("AUCTION_HOUSE_CLOSED"); }));
    s.push_back(events.subscribe<ListChanged>([addon](const ListChanged& e) {
        switch (e.list) {
            case AuctionList::Owned: addon("AUCTION_OWNED_LIST_UPDATE"); break;
            case AuctionList::Bids:  addon("AUCTION_BIDDER_LIST_UPDATE"); break;
            default:                 addon("AUCTION_ITEM_LIST_UPDATE"); break;
        }
    }));
    s.push_back(events.subscribe<ActionSucceeded>([&chat, &frameXml, &inventory, actionName](const ActionSucceeded& e) {
        chat.addSystemChatMessage(std::string("Auction ") + actionName(e.action) + " successful.");
        if (frameXml.frameXmlListening()) {
            frameXml.fireFrameXmlEvent("PLAYER_MONEY", {});
            inventory.fireBagUpdates();
        }
    }));
    s.push_back(events.subscribe<ActionFailed>([&chat, &events, &frameXml, actionName](const ActionFailed& e) {
        const char* reason = "Unknown error.";
        switch (e.error) {
            case 1: reason = "Not enough money."; break;
            case 2: reason = "Item not found."; break;
            case 5: reason = "Bid too low."; break;
            case 6: reason = "Bid increment too low."; break;
            case 7: reason = "You cannot bid on your own auction."; break;
            case 8: reason = "Database error."; break;
            default: break;
        }
        const std::string msg = std::string("Auction ") + actionName(e.action) + " failed: " + reason;
        raiseUiError(events, frameXml, msg);
        chat.addSystemChatMessage(msg);
    }));
    return s;
}

} // namespace wowee::game
