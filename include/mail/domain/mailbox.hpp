#pragma once

#include "mail/domain/events.hpp"
#include "mail/domain/records.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::mail {

/// SMSG_SEND_MAIL_RESULT's action (MailResponseType).
namespace MailAction {
inline constexpr uint32_t Send = 0;
inline constexpr uint32_t MoneyTaken = 1;
inline constexpr uint32_t ItemTaken = 2;
inline constexpr uint32_t ReturnedToSender = 3;
inline constexpr uint32_t Deleted = 4;
inline constexpr uint32_t MadePermanent = 5;
}

/// The mailbox the player is at: the inbox the server last sent, which letter
/// is selected, the letter being written and its enclosures, and whether
/// unread mail is waiting.
class Mailbox {
public:
    /// The player opened a mailbox: unread mail is now in front of them, and
    /// any letter half-written at the last one is gone.
    std::vector<MailChange> opened(uint64_t mailboxGuid);
    /// SMSG_SHOW_MAILBOX: the server opened one.
    std::vector<MailChange> shownByServer(uint64_t mailboxGuid);
    std::vector<MailChange> closed();
    /// A character is logging in: no mailbox, letter or enclosure is theirs.
    void beginCharacter() { *this = Mailbox{}; }

    std::vector<MailChange> inboxReceived(std::vector<MailMessage> inbox);
    /// A letter was read. The server does not send the list again for this,
    /// so the letter is marked here - and the unread flag follows.
    std::vector<MailChange> markedRead(uint32_t mailId);
    /// Whether unread mail is waiting, as SMSG_RECEIVED_MAIL or the next-mail
    /// poll says.
    std::vector<MailChange> newMail(bool waiting);
    std::vector<MailChange> sendResult(uint32_t mailId, uint32_t action, uint32_t error);
    void senderNamed(uint64_t senderGuid, const std::string& name);
    /// An item's template arrived. An auction letter's subject is an item
    /// entry, not words, so a row drawn before the answer is redrawn once it
    /// can name the item.
    std::vector<MailChange> itemInfoArrived(uint32_t itemEntry) const;

    /// Puts an item on the letter in the first free place among `capacity`.
    /// Returns the place, or nothing when the letter is full.
    std::optional<int> enclose(Enclosure item, int capacity, std::vector<MailChange>& changes);
    std::vector<MailChange> removeEnclosure(int index);
    std::vector<MailChange> clearEnclosures();

    /// FrameXML says which of its two tabs shows; the send tab is what makes a
    /// click on a bag item attach it.
    void composeShowing(bool showing) { composing_ = showing; }
    std::vector<MailChange> startComposing();
    std::vector<MailChange> stopComposing();
    void select(int index) { selected_ = index; }

    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] uint64_t mailboxGuid() const { return mailboxGuid_; }
    [[nodiscard]] const std::vector<MailMessage>& inbox() const { return inbox_; }
    [[nodiscard]] int selected() const { return selected_; }
    [[nodiscard]] bool isComposing() const { return composing_; }
    [[nodiscard]] bool hasNewMail() const { return hasNewMail_; }
    [[nodiscard]] const std::array<Enclosure, kMaxEnclosures>& enclosures() const { return enclosures_; }
    [[nodiscard]] int enclosureCount() const;

private:
    void setNewMail(bool waiting, std::vector<MailChange>& changes);
    void clearEnclosures(std::vector<MailChange>& changes);

    bool open_ = false;
    uint64_t mailboxGuid_ = 0;
    std::vector<MailMessage> inbox_;
    int selected_ = -1;
    bool composing_ = false;
    bool hasNewMail_ = false;
    std::array<Enclosure, kMaxEnclosures> enclosures_{};
};

} // namespace wowee::mail
