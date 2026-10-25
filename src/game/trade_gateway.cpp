#include "game/trade_gateway.hpp"

#include "core/cvar_store.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/inventory_handler.hpp"
#include "game/packet_sender.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "trade/domain/events.hpp"

namespace wowee::game {

bool TradeGateway::online() const {
    return sender_.characterInWorld();
}

bool TradeGateway::mayAskToTrade() {
    return sender_.worldCommandAllowed("trade");
}

void TradeGateway::sendInitiate(uint64_t target) {
    sender_.send(InitiateTradePacket::build(target));
    LOG_INFO("Initiated trade with target: 0x", std::hex, target, std::dec);
}

void TradeGateway::sendBegin() {
    sender_.send(BeginTradePacket::build());
}

void TradeGateway::sendCancel() {
    sender_.send(CancelTradePacket::build());
}

void TradeGateway::sendAccept() {
    sender_.send(AcceptTradePacket::build());
}

void TradeGateway::sendUnaccept() {
    sender_.send(UnacceptTradePacket::build());
}

void TradeGateway::sendSetItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) {
    sender_.send(SetTradeItemPacket::build(tradeSlot, bag, bagSlot));
}

void TradeGateway::sendClearItem(uint8_t tradeSlot) {
    sender_.send(ClearTradeItemPacket::build(tradeSlot));
}

void TradeGateway::sendSetGold(uint32_t copper) {
    sender_.send(SetTradeGoldPacket::build(copper));
}

std::string TradeGateway::playerName(uint64_t guid) const {
    return world_.playerName(guid);
}

// Block Trades, which the panel offers.
bool TradeGateway::tradesBlocked() const {
    return cvars_.get("blockTrades", "0") != "0";
}

void TradeGateway::requestItem(uint32_t itemEntry) {
    inventory_.ensureItemInfo(itemEntry);
}

// The codes are AzerothCore's TradeStatus enum, checked against SharedDefines.h
// rather than against the names that used to be here: 7 is BACK_TO_TRADE, an
// acceptance taken back, and 8 is TRADE_COMPLETE.
std::optional<trade::StatusCode> tradeStatusCode(uint32_t status) {
    using trade::StatusCode;
    switch (static_cast<StatusCode>(status)) {
        case StatusCode::Busy: case StatusCode::BeginTrade: case StatusCode::OpenWindow:
        case StatusCode::Canceled: case StatusCode::Accepted: case StatusCode::Busy2:
        case StatusCode::NoTarget: case StatusCode::BackToTrade: case StatusCode::Complete:
        case StatusCode::Rejected: case StatusCode::TooFar: case StatusCode::WrongFaction:
        case StatusCode::CloseWindow: case StatusCode::IgnoreYou: case StatusCode::YouStunned:
        case StatusCode::TargetStunned: case StatusCode::YouDead: case StatusCode::TargetDead:
        case StatusCode::YouLogout: case StatusCode::TargetLogout: case StatusCode::NotOnTapList:
            return static_cast<StatusCode>(status);
    }
    return std::nullopt;
}

// SendUpdateTrade writes:
//
//   uint8  traderData     1 = the other player's offer, 0 = your own
//   uint32 tradeId
//   uint32 slotCount      seven, twice
//   uint32 slotCount
//   uint32 gold           the *header* carries it, not the tail
//   uint32 spell          cast on the lowest slot's item
//   per slot (seven of them):
//     uint8  slotIndex
//     uint32 entry, displayId, stackCount, wrapped
//     uint64 giftCreator
//     uint32 permEnchant, gem1, gem2, gem3
//     uint64 creator
//     uint32 charges, suffixFactor
//     int32  randomPropertyId
//     uint32 lockId, maxDurability, durability
//
// An empty slot writes eighteen zeroed uint32s, which is the same seventy-two
// bytes, so the stride never changes: the header is twenty-one bytes and a
// slot seventy-three.
std::optional<TradeOffer> readTradeOffer(network::Packet& packet) {
    constexpr size_t kHeaderBytes = 1 + 4 * 5;
    constexpr size_t kSlotBytes = 1 + 4 * 4 + 8 + 4 * 4 + 8 + 4 * 6;
    if (!packet.hasRemaining(kHeaderBytes)) { packet.skipAll(); return std::nullopt; }

    TradeOffer offer;
    const uint8_t traderData = packet.readUInt8();
    /*uint32_t tradeId    =*/ packet.readUInt32();
    const uint32_t slotCount = packet.readUInt32();
    /*uint32_t slotCount2 =*/ packet.readUInt32();
    offer.gold = packet.readUInt32();
    /*uint32_t spell     =*/ packet.readUInt32();
    // Zero is this player's own offer.
    offer.ownSide = (traderData == 0);

    uint32_t count = slotCount;
    if (count > static_cast<uint32_t>(trade::kSlotCount)) count = trade::kSlotCount;
    LOG_DEBUG("SMSG_TRADE_STATUS_EXTENDED: ", offer.ownSide ? "own" : "trader",
              " slots=", count, " gold=", offer.gold);

    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(kSlotBytes)) { packet.skipAll(); break; }
        trade::OfferedSlot slot;
        slot.index = packet.readUInt8();
        slot.itemId = packet.readUInt32();
        slot.displayId = packet.readUInt32();
        slot.stackCount = packet.readUInt32();
        /*uint32_t wrapped     =*/ packet.readUInt32();
        /*uint64_t giftCreator =*/ packet.readUInt64();
        /*uint32_t permEnchant =*/ packet.readUInt32();
        for (int g = 0; g < 3; ++g) packet.readUInt32();   // socket enchants
        /*uint64_t creator     =*/ packet.readUInt64();
        /*uint32_t charges     =*/ packet.readUInt32();
        /*uint32_t suffixFactor=*/ packet.readUInt32();
        /*int32_t randomProp   =*/ packet.readUInt32();
        /*uint32_t lockId      =*/ packet.readUInt32();
        /*uint32_t maxDur      =*/ packet.readUInt32();
        /*uint32_t durability  =*/ packet.readUInt32();
        offer.slots.push_back(slot);
    }
    packet.skipAll();
    return offer;
}

namespace {

const char* refusalText(trade::Refusal why) {
    using trade::Refusal;
    switch (why) {
        case Refusal::Busy:          return "Trade failed: player is busy.";
        case Refusal::NoTarget:      return "You have no target.";
        case Refusal::TooFar:        return "Trade failed: target is too far away.";
        case Refusal::WrongFaction:  return "You cannot trade with the enemy.";
        case Refusal::Ignored:       return "That player is ignoring you.";
        case Refusal::LoggingOut:    return "Trade failed: logging out.";
        case Refusal::YouStunned:    return "You are stunned.";
        case Refusal::TargetStunned: return "That player is stunned.";
        case Refusal::YouDead:       return "You are dead.";
        case Refusal::TargetDead:    return "That player is dead.";
        case Refusal::NotOnTapList:  return "You do not have permission to loot that item.";
    }
    return "";
}

/// Said in chat; the rest are interface errors.
bool refusalIsChat(trade::Refusal why) {
    using trade::Refusal;
    return why == Refusal::Busy || why == Refusal::TooFar || why == Refusal::LoggingOut;
}

} // namespace

std::vector<core::EventBus::Subscription>
subscribeTradeNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                      InventoryHandler& inventory) {
    using namespace trade;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto error = [&events, &frameXml](const std::string& text) { raiseUiError(events, frameXml, text); };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    s.push_back(events.subscribe<TradeProposed>([line](const auto&) { line("Requesting trade with target."); }));
    s.push_back(events.subscribe<NoTradeTarget>([error](const auto&) {
        error("You must target a player to trade with.");
    }));
    s.push_back(events.subscribe<TradeRequestBlocked>([line](const TradeRequestBlocked& e) {
        line("Declined a trade from " + e.peerName + " (trades are blocked).");
    }));
    s.push_back(events.subscribe<TradeRequested>([line, addon](const TradeRequested& e) {
        line(e.peerName + " wants to trade with you.");
        addon("TRADE_REQUEST", {e.peerName});
    }));
    s.push_back(events.subscribe<TradeShown>([line, addon](const auto&) {
        line("Trade opened.");
        addon("TRADE_SHOW");
    }));
    s.push_back(events.subscribe<PartnerAccepted>([line](const auto&) { line("Trade partner accepted."); }));
    // TRADE_ACCEPT_UPDATE(playerState, targetState): TradeFrame_SetAcceptState
    // reads both - each decides whether that half is highlighted and whether
    // the trade button is enabled.
    s.push_back(events.subscribe<AcceptStateChanged>([addon](const AcceptStateChanged& e) {
        addon("TRADE_ACCEPT_UPDATE", {e.self ? "1" : "0", e.partner ? "1" : "0"});
    }));
    s.push_back(events.subscribe<TradeEnded>([line, addon, &frameXml, &inventory](const TradeEnded& e) {
        switch (e.how) {
            case Ending::Cancelled: line("Trade cancelled."); break;
            case Ending::Declined:  line("Trade declined."); break;
            case Ending::Completed: line("Trade complete."); break;
            case Ending::WindowClosed: break;
        }
        addon("TRADE_CLOSED");
        if (e.how == Ending::Completed && frameXml.frameXmlListening()) {
            inventory.fireBagUpdates();
            addon("PLAYER_MONEY");
        }
    }));
    s.push_back(events.subscribe<TradeRefused>([line, error](const TradeRefused& e) {
        if (refusalIsChat(e.why)) line(refusalText(e.why));
        else error(refusalText(e.why));
    }));
    s.push_back(events.subscribe<OffersChanged>([addon](const auto&) { addon("TRADE_UPDATE"); }));
    // One event per side, because they are two different frames. A money
    // frame answers only its own name: TARGET_TRADE listens for
    // TRADE_MONEY_CHANGED, PLAYER_TRADE for PLAYER_TRADE_MONEY.
    s.push_back(events.subscribe<OfferedMoneyChanged>([addon](const OfferedMoneyChanged& e) {
        addon(e.own ? "PLAYER_TRADE_MONEY" : "TRADE_MONEY_CHANGED");
    }));
    return s;
}

} // namespace wowee::game
