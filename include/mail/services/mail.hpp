#pragma once

#include "mail/domain/records.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::mail {

/// The mailbox as the interface and the addon layer use it.
class IMailbox {
public:
    virtual ~IMailbox() = default;

    // ---- Reading ----
    [[nodiscard]] virtual bool isMailboxOpen() const = 0;
    [[nodiscard]] virtual const std::vector<MailMessage>& getMailInbox() const = 0;
    [[nodiscard]] virtual int getSelectedMailIndex() const = 0;
    [[nodiscard]] virtual bool isMailComposeOpen() const = 0;
    [[nodiscard]] virtual bool hasNewMail() const = 0;
    [[nodiscard]] virtual const std::array<Enclosure, kMaxEnclosures>& getMailAttachments() const = 0;
    [[nodiscard]] virtual int getMailAttachmentCount() const = 0;
    /// Who a letter is from, resolved now, so a name that arrived late shows.
    [[nodiscard]] virtual std::string getMailSenderName(const MailMessage& mail) const = 0;
    /// The subject as shown: an auction letter names its item.
    virtual std::string getMailDisplaySubject(const MailMessage& mail) = 0;

    // ---- The panel ----
    virtual void setSelectedMailIndex(int index) = 0;
    virtual void openMailCompose() = 0;
    virtual void closeMailCompose() = 0;
    /// Whether the send tab shows, without touching the letter. FrameXML owns
    /// the window and says so as its tabs swap; openMailCompose empties the
    /// enclosures, which is wrong on a tab switch.
    virtual void setMailComposeShowing(bool showing) = 0;

    // ---- Requests ----
    virtual void openMailbox(uint64_t mailboxGuid) = 0;
    virtual void closeMailbox() = 0;
    virtual void refreshMailList() = 0;
    virtual void sendMail(const std::string& recipient, const std::string& subject,
                          const std::string& body, uint64_t money, uint64_t cod = 0) = 0;
    virtual bool attachItemFromBackpack(int backpackIndex) = 0;
    virtual bool attachItemFromBag(int bagIndex, int slotIndex) = 0;
    virtual bool detachMailAttachment(int attachIndex) = 0;
    virtual void clearMailAttachments() = 0;
    virtual void mailTakeMoney(uint32_t mailId) = 0;
    virtual void mailTakeItem(uint32_t mailId, uint32_t itemGuidLow) = 0;
    virtual void mailDelete(uint32_t mailId) = 0;
    virtual void mailReturnToSender(uint32_t mailId) = 0;
    virtual void mailMarkAsRead(uint32_t mailId) = 0;
};

/// `mailbox`, or the mailbox of a client with no game: never open, never
/// holding mail, and every request dropped - for a reader that would
/// otherwise test for it at every call.
[[nodiscard]] IMailbox& orNoMailbox(IMailbox* mailbox);

/// What the protocol layer tells the mailbox.
class IMailboxFeed {
public:
    virtual ~IMailboxFeed() = default;

    virtual void onMailboxShown(uint64_t mailboxGuid) = 0;
    virtual void onInbox(std::vector<MailMessage> inbox) = 0;
    virtual void onSendResult(uint32_t mailId, uint32_t action, uint32_t error) = 0;
    virtual void onNewMail(bool waiting) = 0;
    virtual void onPlayerNameResolved(uint64_t guid, const std::string& name) = 0;
    virtual void onItemInfo(uint32_t itemEntry) = 0;
    /// A character is logging in: none of the last one's mail is theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::mail
