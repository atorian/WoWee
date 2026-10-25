// The mailbox, end to end: the server's mail packets, what the mail frames
// read and are told, the lines the player sees, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/update_field_table.hpp"
#include "mail/domain/events.hpp"
#include "mail/domain/records.hpp"
#include "mail/services/mail.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::game::fieldIndex;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kMailbox = 0xF110000000000B0Bull;
constexpr uint64_t kJaina = 0x21;
constexpr uint64_t kCloth = 0x4000000000000A02ull;
constexpr uint32_t kLinen = 2589;

struct Letter {
    uint32_t id;
    uint8_t type;            // 0 a player, 2 the auction house
    uint64_t sender;         // a guid for a player, an entry otherwise
    std::string subject;
    uint32_t money = 0;
    bool read = false;
};

/// SMSG_MAIL_LIST_RESULT as 3.3.5a writes it: the total and the count shown,
/// then each letter with its size, id, type, sender, the six words and the
/// expiry, the template, subject, body and no attachments.
std::vector<uint8_t> inbox(const std::vector<Letter>& letters) {
    Body b;
    b.u32(static_cast<uint32_t>(letters.size())).u8(static_cast<uint8_t>(letters.size()));
    for (const auto& l : letters) {
        b.u8(0).u8(0);                                  // declared size, advisory
        b.u32(l.id).u8(l.type);
        if (l.type == 0) b.u64(l.sender); else b.u32(static_cast<uint32_t>(l.sender));
        b.u32(0).u32(0).u32(41).u32(l.money).u32(l.read ? 1 : 0).f32(30.0f).u32(0);
        b.cstr(l.subject).cstr("Hello").u8(0);
    }
    return b.bytes();
}

std::vector<uint8_t> sendResult(uint32_t mailId, uint32_t action, uint32_t error) {
    return Body().u32(mailId).u32(action).u32(error).bytes();
}

void atMailbox(GameHarness& h) {
    h.mail().openMailbox(kMailbox);
    h.pump();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

}  // namespace

TEST_CASE("opening a mailbox shows it, empties the letter being written and asks for the inbox",
          "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_RECEIVED_MAIL, Body().f32(0.0f).bytes());
    h.pump();
    REQUIRE(h.mail().hasNewMail());
    h.clearRecords();

    atMailbox(h);

    REQUIRE(h.mail().isMailboxOpen());
    CHECK_FALSE(h.mail().hasNewMail());
    // The envelope, the empty letter, then the frame.
    const size_t pending = h.firedAt("UPDATE_PENDING_MAIL");
    const size_t compose = h.firedAt("MAIL_SEND_INFO_UPDATE");
    const size_t show = h.firedAt("MAIL_SHOW");
    REQUIRE(show < h.addonEvents().size());
    CHECK(pending < compose);
    CHECK(compose < show);
    auto* list = h.lastSent(Opcode::CMSG_GET_MAIL_LIST);
    REQUIRE(list);
    CHECK(list->readUInt64() == kMailbox);

    h.mail().closeMailbox();
    h.pump();
    CHECK_FALSE(h.mail().isMailboxOpen());
    CHECK(h.addonEvents("MAIL_CLOSED").size() == 1);
}

TEST_CASE("a mailbox the server shows asks for its inbox", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_SHOW_MAILBOX, Body().u64(kMailbox).bytes());
    h.pump();

    CHECK(h.mail().isMailboxOpen());
    CHECK(h.addonEvents("MAIL_SHOW").size() == 1);
    auto* list = h.lastSent(Opcode::CMSG_GET_MAIL_LIST);
    REQUIRE(list);
    CHECK(list->readUInt64() == kMailbox);
}

TEST_CASE("the inbox names its senders and its auction letters' items once they are known",
          "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    atMailbox(h);
    h.clearRecords();

    h.receive(Opcode::SMSG_MAIL_LIST_RESULT,
              inbox({{11, 0, kJaina, "Hi"}, {12, 2, 7, std::to_string(kLinen) + ":0:1:5:20"}}));
    h.pump();

    const auto& letters = h.mail().getMailInbox();
    REQUIRE(letters.size() == 2);
    CHECK(h.addonEvents("MAIL_INBOX_UPDATE").size() == 1);
    // A player's letter carries a guid: the name is asked for.
    CHECK(h.lastSent(Opcode::CMSG_NAME_QUERY));
    CHECK(h.mail().getMailSenderName(letters[0]) == "Unknown");
    CHECK(h.mail().getMailSenderName(letters[1]) == "Auction House");
    CHECK(h.mail().getMailDisplaySubject(letters[1]) == "Auction won: Item #2589");

    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE,
              Body().packedGuid(kJaina).u8(0).cstr("Jaina").cstr("").u8(1).u8(1).u8(8).bytes());
    h.pump();
    CHECK(h.mail().getMailInbox()[0].senderName == "Jaina");
    CHECK(h.mail().getMailSenderName(h.mail().getMailInbox()[0]) == "Jaina");

    // The auction letter's item arriving redraws the inbox.
    h.receive(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE, GameHarness::itemTemplateBody(kLinen, "Linen Cloth", 7, 0));
    h.pump();
    CHECK(h.addonEvents("MAIL_INBOX_UPDATE").size() == 2);
    CHECK(h.mail().getMailDisplaySubject(h.mail().getMailInbox()[1]) == "Auction won: Linen Cloth");
}

TEST_CASE("new mail is said once while it waits, and the poll clears it", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    size_t sounds = 0;
    auto sub = h.events().subscribe<wowee::mail::NewMailArrived>([&](const auto&) { ++sounds; });

    h.receive(Opcode::SMSG_RECEIVED_MAIL, Body().f32(0.0f).bytes());
    // MSG_QUERY_NEXT_MAIL_TIME: when, and how many are waiting.
    h.receive(Opcode::MSG_QUERY_NEXT_MAIL_TIME, Body().f32(0.0f).u32(1).bytes());
    h.pump();

    CHECK(h.mail().hasNewMail());
    CHECK(sounds == 1);
    const auto& history = h.game().getChatHistory();
    CHECK(std::count_if(history.begin(), history.end(),
                        [](const auto& l) { return l.message == "You have new mail."; }) == 1);
    CHECK(h.addonEvents("UPDATE_PENDING_MAIL").size() == 2);

    h.receive(Opcode::MSG_QUERY_NEXT_MAIL_TIME, Body().f32(-1.0f).u32(0).bytes());
    h.pump();
    CHECK_FALSE(h.mail().hasNewMail());
}

TEST_CASE("reading the last unread letter puts the envelope away", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    atMailbox(h);
    h.receive(Opcode::SMSG_MAIL_LIST_RESULT, inbox({{11, 0, kJaina, "Hi"}}));
    h.receive(Opcode::SMSG_RECEIVED_MAIL, Body().f32(0.0f).bytes());
    h.pump();
    h.clearRecords();

    h.mail().mailMarkAsRead(11);
    h.pump();

    auto* read = h.lastSent(Opcode::CMSG_MAIL_MARK_AS_READ);
    REQUIRE(read);
    CHECK(read->readUInt64() == kMailbox);
    CHECK(read->readUInt32() == 11);
    CHECK(h.mail().getMailInbox()[0].read);
    CHECK_FALSE(h.mail().hasNewMail());
    CHECK(h.addonEvents("MAIL_INBOX_UPDATE").size() == 1);
    CHECK(h.addonEvents("UPDATE_PENDING_MAIL").size() == 1);
}

TEST_CASE("a letter without a recipient is refused here, and the frame is told", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    atMailbox(h);
    h.clearRecords();

    h.mail().sendMail("", "Hi", "Hello", 0);
    h.pump();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_SEND_MAIL));
    CHECK(chatSaid(h, "Enter a recipient."));
    const auto failed = h.addonEvents("MAIL_FAILED");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].args == std::vector<std::string>{"0"});
}

TEST_CASE("a letter away from a mailbox is refused", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();

    h.mail().sendMail("Jaina", "Hi", "Hello", 0);
    h.pump();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_SEND_MAIL));
    CHECK(chatSaid(h, "You are not at a mailbox."));
}

TEST_CASE("a sent letter is said, the frame resets in the order it always heard, and the inbox is asked again",
          "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    atMailbox(h);
    h.clearRecords();

    h.mail().sendMail("Jaina", "Hi", "Hello", 100);
    REQUIRE(h.lastSent(Opcode::CMSG_SEND_MAIL));
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_SEND_MAIL_RESULT, sendResult(0, 0 /* send */, 0));
    h.pump();

    CHECK(chatSaid(h, "Mail sent."));
    const size_t line = h.firedAt("CHAT_MSG_SYSTEM", "Mail sent.");
    const size_t compose = h.firedAt("MAIL_SEND_INFO_UPDATE");
    const size_t sent = h.firedAt("MAIL_SEND_SUCCESS");
    const size_t success = h.firedAt("MAIL_SUCCESS");
    REQUIRE(success < h.addonEvents().size());
    CHECK(line < compose);
    CHECK(compose < sent);
    CHECK(sent < success);
    CHECK(h.lastSent(Opcode::CMSG_GET_MAIL_LIST));
}

TEST_CASE("the server's refusal is said in words, and the frame hears which", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    atMailbox(h);
    h.clearRecords();

    h.receive(Opcode::SMSG_SEND_MAIL_RESULT, sendResult(0, 0, 19));
    h.receive(Opcode::SMSG_SEND_MAIL_RESULT, sendResult(0, 0, 99));
    h.pump();

    CHECK(chatSaid(h, "Mail not sent: One of the attached items cannot be mailed."));
    CHECK(chatSaid(h, "Failed to send mail (error 99)."));
    const auto failed = h.addonEvents("MAIL_FAILED");
    REQUIRE(failed.size() == 2);
    CHECK(failed[0].args == std::vector<std::string>{"19"});
}

TEST_CASE("a deleted letter closes on screen and leaves the inbox", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    atMailbox(h);
    h.receive(Opcode::SMSG_MAIL_LIST_RESULT, inbox({{11, 0, kJaina, "One"}, {12, 0, kJaina, "Two"}}));
    h.pump();
    h.mail().setSelectedMailIndex(1);
    h.clearRecords();

    h.mail().mailDelete(11);
    h.pump();
    auto* del = h.lastSent(Opcode::CMSG_MAIL_DELETE);
    REQUIRE(del);
    CHECK(del->readUInt64() == kMailbox);
    CHECK(del->readUInt32() == 11);
    const auto closed = h.addonEvents("CLOSE_INBOX_ITEM");
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].args == std::vector<std::string>{"11"});

    h.receive(Opcode::SMSG_SEND_MAIL_RESULT, sendResult(11, 4 /* deleted */, 0));
    h.pump();
    CHECK(chatSaid(h, "Mail deleted."));
    REQUIRE(h.mail().getMailInbox().size() == 1);
    CHECK(h.mail().getMailInbox()[0].messageId == 12);
    // The selected letter was the second; it is now the first.
    CHECK(h.mail().getSelectedMailIndex() == 0);
}

TEST_CASE("money taken from a letter empties its purse", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    Letter withGold{11, 0, kJaina, "Gold"};
    withGold.money = 5000;
    atMailbox(h);
    h.receive(Opcode::SMSG_MAIL_LIST_RESULT, inbox({withGold}));
    h.pump();
    h.clearRecords();

    h.mail().mailTakeMoney(11);
    CHECK(h.lastSent(Opcode::CMSG_MAIL_TAKE_MONEY));
    h.receive(Opcode::SMSG_SEND_MAIL_RESULT, sendResult(11, 1 /* money taken */, 0));
    h.pump();

    CHECK(h.mail().getMailInbox()[0].money == 0);
    CHECK(chatSaid(h, "Money taken from mail."));
    CHECK(h.addonEvents("PLAYER_MONEY").size() == 1);
}

TEST_CASE("an item in the backpack goes on the letter and with it", "[mail][harness]") {
    GameHarness h;
    h.enterWorld();
    h.playerInView();
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::createItemBody(kCloth, kLinen, 7));
    GameHarness::Fields pack;
    GameHarness::putGuid(pack, static_cast<uint16_t>(fieldIndex(wowee::game::UF::PLAYER_FIELD_PACK_SLOT_1)), kCloth);
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(h.player().guid, pack));
    h.pump();
    atMailbox(h);
    h.clearRecords();

    REQUIRE(h.mail().attachItemFromBackpack(0));
    h.pump();

    CHECK(h.mail().getMailAttachmentCount() == 1);
    const auto& enclosure = h.mail().getMailAttachments()[0];
    CHECK(enclosure.itemGuid == kCloth);
    CHECK(enclosure.itemId == kLinen);
    CHECK(enclosure.stackCount == 7);
    CHECK(h.addonEvents("MAIL_SEND_INFO_UPDATE").size() == 1);

    h.mail().sendMail("Jaina", "Cloth", "For you", 0);
    REQUIRE(h.lastSent(Opcode::CMSG_SEND_MAIL));
    // Sent, the letter being written is empty again.
    CHECK(h.mail().getMailAttachmentCount() == 0);
}

TEST_CASE("the next character is at no mailbox and has none of the last one's mail", "[mail][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    atMailbox(h);
    h.receive(Opcode::SMSG_MAIL_LIST_RESULT, inbox({{11, 0, kJaina, "Hi"}}));
    h.receive(Opcode::SMSG_RECEIVED_MAIL, Body().f32(0.0f).bytes());
    h.pump();
    REQUIRE(h.mail().hasNewMail());

    h.relogAs(bob);

    CHECK_FALSE(h.mail().isMailboxOpen());
    CHECK(h.mail().getMailInbox().empty());
    CHECK_FALSE(h.mail().hasNewMail());
    h.mail().mailDelete(11);
    CHECK_FALSE(h.lastSent(Opcode::CMSG_MAIL_DELETE));
}
