#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::mail {

// What happened at the mailbox. Mailbox returns these; the service publishes
// them, and the chat log, the addon layer and the sound subscribe.
struct MailboxOpened {};
struct MailboxClosed {};
/// The inbox's letters changed (MAIL_INBOX_UPDATE).
struct InboxChanged {};
/// The letter being written changed (MAIL_SEND_INFO_UPDATE).
struct ComposeChanged {};
/// Unread mail appeared where there was none.
struct NewMailArrived {};
/// Whether there is unread mail was said again (UPDATE_PENDING_MAIL).
struct PendingMailChanged {};
/// The server took the letter.
struct MailSent {};
/// The send frame may clear its fields: after a letter went and its
/// enclosures were emptied (MAIL_SEND_SUCCESS).
struct SendFrameReset {};
struct SendFailed { uint32_t error = 0; };
struct ItemTaken {};
struct TakeItemFailed { uint32_t error = 0; };
struct MoneyTaken {};
struct TakeMoneyFailed { uint32_t error = 0; };
struct MailRemoved { bool returned = false; };
struct TextCopied {};

using MailChange = std::variant<MailboxOpened, MailboxClosed, InboxChanged, ComposeChanged,
                                NewMailArrived, PendingMailChanged, MailSent, SendFrameReset,
                                SendFailed, ItemTaken, TakeItemFailed, MoneyTaken, TakeMoneyFailed,
                                MailRemoved, TextCopied>;

// What became of a request the player made, published by the service.

/// A letter the client refused to send, with the reason for the player. The
/// interface hears MAIL_FAILED, so its Send button returns.
struct SendRefused { std::string reason; };
/// The letter on screen is gone (CLOSE_INBOX_ITEM).
struct InboxItemClosed { uint32_t mailId = 0; };
/// An enclosed item can still be returned to its vendor, which posting it
/// ends (MAIL_LOCK_SEND_ITEMS). `slot` counts from zero.
struct EnclosureRefundable { int slot = 0; uint32_t itemId = 0; std::string name; };

} // namespace wowee::mail
