#include "mail/services/mail_service.hpp"

#include "foundation/event_bus.hpp"
#include "foundation/logger.hpp"

namespace wowee::mail {

namespace {

/// The mailbox of a client with no game: never open, never holding mail, and
/// every request dropped.
class NoMailbox final : public IMailbox {
public:
    bool isMailboxOpen() const override { return false; }
    const std::vector<MailMessage>& getMailInbox() const override { return inbox_; }
    int getSelectedMailIndex() const override { return -1; }
    bool isMailComposeOpen() const override { return false; }
    bool hasNewMail() const override { return false; }
    const std::array<Enclosure, kMaxEnclosures>& getMailAttachments() const override { return enclosures_; }
    int getMailAttachmentCount() const override { return 0; }
    std::string getMailSenderName(const MailMessage&) const override { return "Unknown"; }
    std::string getMailDisplaySubject(const MailMessage& mail) override { return mail.subject; }
    void setSelectedMailIndex(int) override {}
    void openMailCompose() override {}
    void closeMailCompose() override {}
    void setMailComposeShowing(bool) override {}
    void openMailbox(uint64_t) override {}
    void closeMailbox() override {}
    void refreshMailList() override {}
    void sendMail(const std::string&, const std::string&, const std::string&, uint64_t, uint64_t) override {}
    bool attachItemFromBackpack(int) override { return false; }
    bool attachItemFromBag(int, int) override { return false; }
    bool detachMailAttachment(int) override { return false; }
    void clearMailAttachments() override {}
    void mailTakeMoney(uint32_t) override {}
    void mailTakeItem(uint32_t, uint32_t) override {}
    void mailDelete(uint32_t) override {}
    void mailReturnToSender(uint32_t) override {}
    void mailMarkAsRead(uint32_t) override {}

private:
    std::vector<MailMessage> inbox_;
    std::array<Enclosure, kMaxEnclosures> enclosures_{};
};

NoMailbox kNoMailbox;

} // namespace

IMailbox& orNoMailbox(IMailbox* mailbox) {
    return mailbox ? *mailbox : kNoMailbox;
}

MailService::MailService(IMailGateway& gateway, IMailWorld& world, core::EventBus& events)
    : gateway_(gateway), world_(world), events_(events) {}

bool MailService::ready() const {
    return gateway_.inWorld() && gateway_.connected() && box_.mailboxGuid() != 0;
}

void MailService::apply(const std::vector<MailChange>& changes) {
    for (const auto& change : changes)
        std::visit([this](const auto& e) { events_.publish(e); }, change);
}

void MailService::openMailbox(uint64_t mailboxGuid) {
    apply(box_.opened(mailboxGuid));
    world_.chooseDefaultStationery();
    refreshMailList();
}

void MailService::onMailboxShown(uint64_t mailboxGuid) {
    apply(box_.shownByServer(mailboxGuid));
    world_.chooseDefaultStationery();
    refreshMailList();
}

void MailService::refreshMailList() {
    if (!box_.isOpen() || !ready()) return;
    gateway_.sendListRequest(box_.mailboxGuid());
}

void MailService::refuse(std::string reason, std::string logLine) {
    // The compose frame disables Send when it is pressed and enables it again
    // only when it hears how the send went; a silent refusal left it disabled
    // for the rest of the session.
    LOG_WARNING("sendMail: ", logLine);
    events_.publish(SendRefused{std::move(reason)});
}

void MailService::sendMail(const std::string& recipient, const std::string& subject,
                           const std::string& body, uint64_t money, uint64_t cod) {
    if (!gateway_.inWorld()) return refuse("Cannot send mail right now.", "not in world");
    if (!gateway_.connected()) return refuse("Cannot send mail: not connected.", "no socket");
    if (box_.mailboxGuid() == 0) return refuse("You are not at a mailbox.", "mailboxGuid_ is 0 (mailbox closed?)");
    if (recipient.empty()) return refuse("Enter a recipient.", "no recipient");

    std::vector<uint64_t> itemGuids;
    for (const auto& e : box_.enclosures())
        if (e.occupied()) itemGuids.push_back(e.itemGuid);
    // Unreachable while enclosing is capped, but dropping enclosures without a
    // word is how it would go unnoticed.
    if (static_cast<int>(itemGuids.size()) > gateway_.maxEnclosures()) {
        LOG_ERROR("sendMail: ", itemGuids.size(), " attachments but this expansion's "
                  "packet carries ", gateway_.maxEnclosures(), " - refusing to send and lose the rest");
        return refuse("This realm's mail carries one item per letter.", "too many attachments");
    }

    gateway_.sendLetter(box_.mailboxGuid(), recipient, subject, body, money, cod, itemGuids);
    apply(box_.clearEnclosures());
}

bool MailService::enclose(std::optional<Enclosure> item) {
    if (!item) return false;
    const uint64_t guid = item->itemGuid;
    const uint32_t itemId = item->itemId;
    const std::string name = item->name;
    std::vector<MailChange> changes;
    const auto slot = box_.enclose(std::move(*item), gateway_.maxEnclosures(), changes);
    if (!slot) return false;
    // Posting an item ends its refund window, so the interface asks first.
    if (world_.refundWindow(guid) == IMailWorld::Refund::Open)
        events_.publish(EnclosureRefundable{*slot, itemId, name});
    apply(changes);
    return true;
}

bool MailService::detachMailAttachment(int attachIndex) {
    if (attachIndex < 0 || attachIndex >= kMaxEnclosures) return false;
    apply(box_.removeEnclosure(attachIndex));
    return true;
}

void MailService::mailTakeMoney(uint32_t mailId) {
    if (ready()) gateway_.sendTakeMoney(box_.mailboxGuid(), mailId);
}

void MailService::mailTakeItem(uint32_t mailId, uint32_t itemGuidLow) {
    if (ready()) gateway_.sendTakeItem(box_.mailboxGuid(), mailId, itemGuidLow);
}

void MailService::mailDelete(uint32_t mailId) {
    if (!ready()) return;
    gateway_.sendDelete(box_.mailboxGuid(), mailId);
    // The letter being read is this one; the view over it hides when told.
    events_.publish(InboxItemClosed{mailId});
}

void MailService::mailReturnToSender(uint32_t mailId) {
    if (!ready()) return;
    gateway_.sendReturn(box_.mailboxGuid(), mailId);
    events_.publish(InboxItemClosed{mailId});
}

void MailService::mailMarkAsRead(uint32_t mailId) {
    if (!ready()) return;
    gateway_.sendMarkRead(box_.mailboxGuid(), mailId);
    apply(box_.markedRead(mailId));
}

void MailService::onInbox(std::vector<MailMessage> inbox) {
    apply(box_.inboxReceived(std::move(inbox)));
    for (const auto& mail : box_.inbox()) {
        // A player's letter carries a guid, not a name; ask once, and the name
        // shows as soon as the answer comes.
        if (mail.messageType == 0 && mail.senderGuid != 0 && world_.playerName(mail.senderGuid).empty())
            world_.requestPlayerName(mail.senderGuid);
        if (mail.messageType == 2) {
            AuctionMailSubject auction;
            if (parseAuctionMailSubject(mail.subject, auction)) world_.requestItem(auction.itemEntry);
        }
        for (const auto& att : mail.attachments)
            if (att.itemId != 0) world_.requestItem(att.itemId);
    }
}

void MailService::onSendResult(uint32_t mailId, uint32_t action, uint32_t error) {
    apply(box_.sendResult(mailId, action, error));
    refreshMailList();
}

std::string MailService::getMailSenderName(const MailMessage& mail) const {
    switch (mail.messageType) {
        case 0: {  // another player
            // The name query fills senderName, which Reply uses; the cache
            // covers the moment between the inbox arriving and the answer.
            if (!mail.senderName.empty()) return mail.senderName;
            if (mail.senderGuid == 0) break;
            if (std::string name = world_.playerName(mail.senderGuid); !name.empty()) return name;
            break;
        }
        case 2:    // the auction house, which has no creature behind it
            return "Auction House";
        case 3:
            if (std::string name = world_.creatureName(mail.senderEntry); !name.empty()) return name;
            break;
        case 4:
            if (std::string name = world_.gameObjectName(mail.senderEntry); !name.empty()) return name;
            break;
        default:
            break;
    }
    return "Unknown";
}

std::string MailService::getMailDisplaySubject(const MailMessage& mail) {
    if (mail.messageType != 2) return mail.subject;
    AuctionMailSubject auction;
    if (!parseAuctionMailSubject(mail.subject, auction)) return mail.subject;
    world_.requestItem(auction.itemEntry);
    std::string itemName = world_.itemName(auction.itemEntry);
    if (itemName.empty()) itemName = "Item #" + std::to_string(auction.itemEntry);
    return formatAuctionMailSubject(auction, itemName);
}

} // namespace wowee::mail
