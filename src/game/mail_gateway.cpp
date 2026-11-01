#include "game/mail_gateway.hpp"

#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/query_cache_handler.hpp"
#include "game/game_utils.hpp"
#include "game/inventory.hpp"
#include "game/inventory_handler.hpp"
#include "game/packet_parsers.hpp"
#include "game/packet_sender.hpp"
#include "game/world_packets.hpp"
#include "mail/domain/events.hpp"
#include "mail/domain/records.hpp"

namespace wowee::game {

namespace {

mail::Enclosure enclosureOf(uint64_t guid, const ItemDef& item, uint8_t srcBag, uint8_t srcSlot) {
    mail::Enclosure e;
    e.itemGuid = guid;
    e.itemId = item.itemId;
    e.stackCount = item.stackCount;
    e.displayInfoId = item.displayInfoId;
    e.name = item.name;
    e.srcBag = srcBag;
    e.srcSlot = srcSlot;
    return e;
}

/// What the server's mail refusal means, in words.
///
/// SMSG_SEND_MAIL_RESULT carries a number from MailResponseResult and the
/// client printed it bare - "Failed to send mail (error 19)" says nothing
/// about what to do differently, and 19 is the one a player can actually act
/// on: something on the letter cannot be mailed.
const char* mailResultText(uint32_t error) {
    switch (error) {
        case 1:  return "Those items cannot go in your bags.";
        case 2:  return "You cannot send mail to yourself.";
        case 3:  return "You do not have enough money.";
        case 4:  return "No player by that name.";
        case 5:  return "You cannot send mail to the other faction.";
        case 6:  return "The server refused the letter.";
        case 14: return "Trial accounts cannot send mail.";
        case 15: return "That player's mailbox is full.";
        case 16: return "A wrapped item cannot be sent cash on delivery.";
        case 17: return "Your mail and chat are suspended.";
        case 18: return "Too many items attached.";
        case 19: return "One of the attached items cannot be mailed.";
        case 21: return "One of the attached items has expired.";
        default: return nullptr;
    }
}

} // namespace

bool MailGateway::inWorld() const {
    return sender_.characterInWorld();
}

bool MailGateway::connected() const {
    return sender_.connected();
}

int MailGateway::maxEnclosures() const {
    // Vanilla's packet has one uint64 item GUID where TBC and later have a
    // count followed by an array, so anything past the first would be left in
    // the player's bags while the panel accepted twelve.
    return isClassicLikeExpansion() ? 1 : mail::kMaxEnclosures;
}

void MailGateway::sendListRequest(uint64_t mailbox) {
    sender_.send(GetMailListPacket::build(mailbox));
}

void MailGateway::sendLetter(uint64_t mailbox, const std::string& recipient, const std::string& subject,
                             const std::string& body, uint64_t money, uint64_t cod,
                             const std::vector<uint64_t>& itemGuids) {
    auto packet = world_.packetParsers().buildSendMail(mailbox, recipient, subject, body, money, cod, itemGuids);
    LOG_INFO("sendMail: to='", recipient, "' subject='", subject, "' money=", money,
             " attachments=", itemGuids.size(), " mailboxGuid=", mailbox);
    sender_.send(packet);
}

void MailGateway::sendTakeMoney(uint64_t mailbox, uint32_t mailId) {
    sender_.send(MailTakeMoneyPacket::build(mailbox, mailId));
}

void MailGateway::sendTakeItem(uint64_t mailbox, uint32_t mailId, uint32_t itemGuidLow) {
    sender_.send(MailTakeItemPacket::build(mailbox, mailId, itemGuidLow));
}

void MailGateway::sendReturn(uint64_t mailbox, uint32_t mailId) {
    sender_.send(MailReturnToSenderPacket::build(mailbox, mailId));
}

void MailGateway::sendDelete(uint64_t mailbox, uint32_t mailId) {
    sender_.send(MailDeletePacket::build(mailbox, mailId, 0));
}

void MailGateway::sendMarkRead(uint64_t mailbox, uint32_t mailId) {
    sender_.send(MailMarkAsReadPacket::build(mailbox, mailId));
}

std::optional<mail::Enclosure> MailGateway::backpackItem(int index) const {
    const auto& items = inventory_.items();
    if (index < 0 || index >= items.getBackpackSize()) return std::nullopt;
    const auto& slot = items.getBackpackSlot(index);
    if (slot.empty()) return std::nullopt;
    const uint64_t guid = inventory_.getBackpackItemGuid(index);
    if (guid == 0) return std::nullopt;
    return enclosureOf(guid, slot.item, 0xFF, static_cast<uint8_t>(Inventory::NUM_EQUIP_SLOTS + index));
}

std::optional<mail::Enclosure> MailGateway::bagItem(int bag, int slotIndex) const {
    const auto& items = inventory_.items();
    if (bag < 0 || bag >= Inventory::NUM_BAG_SLOTS) return std::nullopt;
    if (slotIndex < 0 || slotIndex >= items.getBagSize(bag)) return std::nullopt;
    const auto& slot = items.getBagSlot(bag, slotIndex);
    if (slot.empty()) return std::nullopt;
    const uint64_t guid = inventory_.getBagItemGuid(bag, slotIndex);
    if (guid == 0) return std::nullopt;
    return enclosureOf(guid, slot.item, static_cast<uint8_t>(Inventory::FIRST_BAG_EQUIP_SLOT + bag),
                       static_cast<uint8_t>(slotIndex));
}

// Only tellable since the refund info is asked for and kept. Not asked yet:
// ask, so the next attachment of the same item knows, and say nothing now
// rather than guess - which is what an item with no window looks like too.
mail::IMailWorld::Refund MailGateway::refundWindow(uint64_t itemGuid) {
    const auto* refund = inventory_.getItemRefundInfo(itemGuid);
    if (!refund) {
        inventory_.requestItemRefundInfo(itemGuid);
        return Refund::Unknown;
    }
    constexpr uint32_t kRefundWindow = 2 * 60 * 60;
    return refund->playedSincePurchase >= kRefundWindow ? Refund::Over : Refund::Open;
}

void MailGateway::chooseDefaultStationery() {
    // The paper a letter is written on, chosen the way it arrives already in
    // the frame.
    //
    // SendMailFrame_CanSend counts three things before it enables the Send
    // button, and one is that a stationery has been chosen. Nothing chooses one
    // when the frame opens: SendMailFrame_Reset is the only thing in the whole
    // interface that does, and it runs after a letter has been sent
    // successfully. So the first letter of a session could never be sent.
    world_.runInterfaceCommand(
        "if StationeryPopupFrame and not StationeryPopupFrame.selectedIndex then "
        "StationeryPopupButton_OnClick(nil, 1) end");
}

std::string MailGateway::playerName(uint64_t guid) const {
    return world_.playerName(guid);
}

std::string MailGateway::creatureName(uint32_t entry) const {
    return names_.getCachedCreatureName(entry);
}

std::string MailGateway::gameObjectName(uint32_t entry) const {
    const auto* info = names_.getCachedGameObjectInfo(entry);
    return info ? info->name : std::string();
}

std::string MailGateway::itemName(uint32_t itemEntry) const {
    const auto* info = inventory_.getItemInfo(itemEntry);
    return info ? info->name : std::string();
}

void MailGateway::requestItem(uint32_t itemEntry) {
    inventory_.ensureItemInfo(itemEntry);
}

void MailGateway::requestPlayerName(uint64_t guid) {
    world_.requestPlayerName(guid);
}

std::vector<core::EventBus::Subscription>
subscribeMailNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                     InventoryHandler& inventory) {
    using namespace mail;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    s.push_back(events.subscribe<MailboxOpened>([addon](const auto&) { addon("MAIL_SHOW"); }));
    s.push_back(events.subscribe<MailboxClosed>([addon](const auto&) { addon("MAIL_CLOSED"); }));
    s.push_back(events.subscribe<InboxChanged>([addon](const auto&) { addon("MAIL_INBOX_UPDATE"); }));
    // The send frame recomputes its slots, its postage and its Send button
    // from this and from nothing else - there is no poll behind it.
    s.push_back(events.subscribe<ComposeChanged>([addon](const auto&) { addon("MAIL_SEND_INFO_UPDATE"); }));
    // Once, on the rising edge: the next-mail poll repeats itself while mail
    // sits unread. The sound is the client's, which hears the same event.
    s.push_back(events.subscribe<NewMailArrived>([line](const auto&) { line("You have new mail."); }));
    // MiniMapMailFrame answers this alone, not the inbox update.
    s.push_back(events.subscribe<PendingMailChanged>([addon](const auto&) { addon("UPDATE_PENDING_MAIL"); }));
    s.push_back(events.subscribe<MailSent>([line](const auto&) { line("Mail sent."); }));
    // The send frame waits on these before it will clear its fields and
    // re-enable the Send button.
    s.push_back(events.subscribe<SendFrameReset>([addon](const auto&) {
        addon("MAIL_SEND_SUCCESS");
        addon("MAIL_SUCCESS");
    }));
    s.push_back(events.subscribe<SendFailed>([line, addon](const SendFailed& e) {
        const char* why = mailResultText(e.error);
        line(why ? std::string("Mail not sent: ") + why
                 : "Failed to send mail (error " + std::to_string(e.error) + ").");
        // Carries the error so the frame can say which refusal it was.
        addon("MAIL_FAILED", {std::to_string(e.error)});
    }));
    s.push_back(events.subscribe<SendRefused>([line, addon](const SendRefused& e) {
        line(e.reason);
        // Zero is "internal error" in the server's own table, the honest
        // answer for a refusal that never reached it.
        addon("MAIL_FAILED", {"0"});
    }));
    s.push_back(events.subscribe<ItemTaken>([line, &frameXml, &inventory](const auto&) {
        line("Item taken from mail.");
        if (frameXml.frameXmlListening()) inventory.fireBagUpdates();
    }));
    s.push_back(events.subscribe<TakeItemFailed>([line](const TakeItemFailed& e) {
        line("Failed to take item (error " + std::to_string(e.error) + ").");
    }));
    s.push_back(events.subscribe<MoneyTaken>([line, addon](const auto&) {
        line("Money taken from mail.");
        addon("PLAYER_MONEY");
    }));
    s.push_back(events.subscribe<TakeMoneyFailed>([line](const TakeMoneyFailed& e) {
        line("Failed to take money (error " + std::to_string(e.error) + ").");
    }));
    s.push_back(events.subscribe<MailRemoved>([line](const MailRemoved& e) {
        line(e.returned ? "Mail returned." : "Mail deleted.");
    }));
    s.push_back(events.subscribe<TextCopied>([line](const auto&) { line("Mail text copied."); }));
    // The letter being read is this one, and the view over it hides when told
    // which mail closed.
    s.push_back(events.subscribe<InboxItemClosed>([addon](const InboxItemClosed& e) {
        addon("CLOSE_INBOX_ITEM", {std::to_string(e.mailId)});
    }));
    // The interface answers with RespondMailLockSendItem(slot, keep).
    s.push_back(events.subscribe<EnclosureRefundable>([addon, &inventory](const EnclosureRefundable& e) {
        const auto* info = inventory.getItemInfo(e.itemId);
        addon("MAIL_LOCK_SEND_ITEMS",
              {std::to_string(e.slot + 1),
               info ? buildItemLink(e.itemId, info->quality, info->name) : e.name});
    }));
    return s;
}

} // namespace wowee::game
