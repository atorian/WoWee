#include "mail/domain/mailbox.hpp"

#include <algorithm>

namespace wowee::mail {

void Mailbox::setNewMail(bool waiting, std::vector<MailChange>& changes) {
    // Announced only when it starts: the next-mail poll repeats itself while
    // mail sits unread, and the announcement must not.
    if (waiting && !hasNewMail_) changes.emplace_back(NewMailArrived{});
    hasNewMail_ = waiting;
    changes.emplace_back(PendingMailChanged{});
}

void Mailbox::clearEnclosures(std::vector<MailChange>& changes) {
    enclosures_.fill(Enclosure{});
    changes.emplace_back(ComposeChanged{});
}

std::vector<MailChange> Mailbox::opened(uint64_t mailboxGuid) {
    std::vector<MailChange> changes;
    mailboxGuid_ = mailboxGuid;
    open_ = true;
    setNewMail(false, changes);
    selected_ = -1;
    composing_ = false;
    clearEnclosures(changes);
    changes.emplace_back(MailboxOpened{});
    return changes;
}

std::vector<MailChange> Mailbox::shownByServer(uint64_t mailboxGuid) {
    mailboxGuid_ = mailboxGuid;
    open_ = true;
    selected_ = -1;
    return {MailboxOpened{}};
}

std::vector<MailChange> Mailbox::closed() {
    std::vector<MailChange> changes;
    open_ = false;
    mailboxGuid_ = 0;
    composing_ = false;
    clearEnclosures(changes);
    changes.emplace_back(MailboxClosed{});
    return changes;
}

std::vector<MailChange> Mailbox::inboxReceived(std::vector<MailMessage> inbox) {
    inbox_ = std::move(inbox);
    return {InboxChanged{}};
}

std::vector<MailChange> Mailbox::markedRead(uint32_t mailId) {
    std::vector<MailChange> changes;
    auto it = std::find_if(inbox_.begin(), inbox_.end(),
                           [mailId](const MailMessage& m) { return m.messageId == mailId && !m.read; });
    if (it == inbox_.end()) return changes;
    // Marked before the inbox is said to have changed: the interface answers
    // that by reading the letter's text, which is what marks it read, and
    // finding it already read is what stops it asking again.
    it->read = true;
    changes.emplace_back(InboxChanged{});
    const bool anyUnread = std::any_of(inbox_.begin(), inbox_.end(), [](const MailMessage& m) { return !m.read; });
    if (anyUnread != hasNewMail_) setNewMail(anyUnread, changes);
    return changes;
}

std::vector<MailChange> Mailbox::newMail(bool waiting) {
    std::vector<MailChange> changes;
    setNewMail(waiting, changes);
    return changes;
}

std::vector<MailChange> Mailbox::sendResult(uint32_t mailId, uint32_t action, uint32_t error) {
    std::vector<MailChange> changes;
    auto letter = std::find_if(inbox_.begin(), inbox_.end(),
                               [mailId](const MailMessage& m) { return m.messageId == mailId; });
    switch (action) {
        case MailAction::Send:
            if (error != 0) { changes.emplace_back(SendFailed{error}); break; }
            // The send frame stays open with its fields reset, as WoW leaves it.
            changes.emplace_back(MailSent{});
            clearEnclosures(changes);
            changes.emplace_back(SendFrameReset{});
            break;
        case MailAction::ItemTaken:
            if (error != 0) changes.emplace_back(TakeItemFailed{error});
            else changes.emplace_back(ItemTaken{});
            break;
        case MailAction::MoneyTaken:
            if (error != 0) { changes.emplace_back(TakeMoneyFailed{error}); break; }
            if (letter != inbox_.end()) letter->money = 0;
            changes.emplace_back(MoneyTaken{});
            changes.emplace_back(InboxChanged{});
            break;
        case MailAction::Deleted:
        case MailAction::ReturnedToSender:
            if (error != 0) break;
            changes.emplace_back(MailRemoved{action == MailAction::ReturnedToSender});
            if (letter != inbox_.end()) {
                const int erased = static_cast<int>(std::distance(inbox_.begin(), letter));
                inbox_.erase(letter);
                if (selected_ == erased) selected_ = -1;
                else if (selected_ > erased) --selected_;
            }
            changes.emplace_back(InboxChanged{});
            break;
        case MailAction::MadePermanent:
            if (error == 0) changes.emplace_back(TextCopied{});
            break;
        default:
            break;
    }
    return changes;
}

void Mailbox::senderNamed(uint64_t senderGuid, const std::string& name) {
    for (auto& m : inbox_)
        if (m.messageType == 0 && m.senderGuid == senderGuid) m.senderName = name;
}

std::vector<MailChange> Mailbox::itemInfoArrived(uint32_t itemEntry) const {
    for (const auto& m : inbox_) {
        AuctionMailSubject auction;
        if (m.messageType == 2 && parseAuctionMailSubject(m.subject, auction) && auction.itemEntry == itemEntry)
            return {InboxChanged{}};
    }
    return {};
}

std::optional<int> Mailbox::enclose(Enclosure item, int capacity, std::vector<MailChange>& changes) {
    const int limit = std::min(capacity, kMaxEnclosures);
    for (int i = 0; i < limit; ++i) {
        if (enclosures_[static_cast<size_t>(i)].occupied()) continue;
        enclosures_[static_cast<size_t>(i)] = std::move(item);
        changes.emplace_back(ComposeChanged{});
        return i;
    }
    return std::nullopt;
}

std::vector<MailChange> Mailbox::removeEnclosure(int index) {
    if (index < 0 || index >= kMaxEnclosures) return {};
    enclosures_[static_cast<size_t>(index)] = Enclosure{};
    return {ComposeChanged{}};
}

std::vector<MailChange> Mailbox::clearEnclosures() {
    std::vector<MailChange> changes;
    clearEnclosures(changes);
    return changes;
}

std::vector<MailChange> Mailbox::startComposing() {
    composing_ = true;
    return clearEnclosures();
}

std::vector<MailChange> Mailbox::stopComposing() {
    composing_ = false;
    return clearEnclosures();
}

int Mailbox::enclosureCount() const {
    return static_cast<int>(std::count_if(enclosures_.begin(), enclosures_.end(),
                                          [](const Enclosure& e) { return e.occupied(); }));
}

} // namespace wowee::mail
