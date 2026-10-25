// The mailbox: its domain model, and the service against a recording gateway
// and the real event bus. Only wowee_mail is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "mail/services/mail_service.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace wowee::mail;

namespace {

template <typename E>
bool has(const std::vector<MailChange>& changes) {
    return std::any_of(changes.begin(), changes.end(),
                       [](const MailChange& c) { return std::holds_alternative<E>(c); });
}

MailMessage letter(uint32_t id, bool read = false, uint8_t type = 0) {
    MailMessage m;
    m.messageId = id;
    m.read = read;
    m.messageType = type;
    return m;
}

Enclosure item(uint64_t guid) {
    Enclosure e;
    e.itemGuid = guid;
    e.itemId = static_cast<uint32_t>(guid);
    return e;
}

struct RecordingGateway final : IMailGateway, IMailWorld {
    bool world = true, socket = true;
    int capacity = kMaxEnclosures;
    std::vector<std::string> sent;
    std::map<uint64_t, std::string> players;
    std::map<uint32_t, std::string> items;
    std::map<uint64_t, Refund> refunds;
    std::vector<std::string> asked;

    bool inWorld() const override { return world; }
    bool connected() const override { return socket; }
    int maxEnclosures() const override { return capacity; }
    void sendListRequest(uint64_t) override { sent.push_back("list"); }
    void sendLetter(uint64_t, const std::string& to, const std::string&, const std::string&,
                    uint64_t, uint64_t, const std::vector<uint64_t>& guids) override {
        sent.push_back("letter " + to + " items " + std::to_string(guids.size()));
    }
    void sendTakeMoney(uint64_t, uint32_t id) override { sent.push_back("money " + std::to_string(id)); }
    void sendTakeItem(uint64_t, uint32_t id, uint32_t) override { sent.push_back("item " + std::to_string(id)); }
    void sendReturn(uint64_t, uint32_t id) override { sent.push_back("return " + std::to_string(id)); }
    void sendDelete(uint64_t, uint32_t id) override { sent.push_back("delete " + std::to_string(id)); }
    void sendMarkRead(uint64_t, uint32_t id) override { sent.push_back("read " + std::to_string(id)); }

    std::optional<Enclosure> backpackItem(int index) const override {
        if (index < 0) return std::nullopt;
        return item(static_cast<uint64_t>(100 + index));
    }
    std::optional<Enclosure> bagItem(int, int) const override { return std::nullopt; }
    Refund refundWindow(uint64_t guid) override {
        auto it = refunds.find(guid);
        return it == refunds.end() ? Refund::Unknown : it->second;
    }
    void chooseDefaultStationery() override {}
    std::string playerName(uint64_t guid) const override {
        auto it = players.find(guid);
        return it == players.end() ? std::string() : it->second;
    }
    std::string creatureName(uint32_t) const override { return {}; }
    std::string gameObjectName(uint32_t) const override { return {}; }
    std::string itemName(uint32_t entry) const override {
        auto it = items.find(entry);
        return it == items.end() ? std::string() : it->second;
    }
    void requestItem(uint32_t entry) override { asked.push_back("item " + std::to_string(entry)); }
    void requestPlayerName(uint64_t guid) override { asked.push_back("name " + std::to_string(guid)); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    MailService service{gateway, gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<SendRefused>([this](const SendRefused& e) { events.push_back("refused: " + e.reason); }));
        subscriptions.push_back(bus.subscribe<InboxItemClosed>([this](const InboxItemClosed& e) { events.push_back("closed " + std::to_string(e.mailId)); }));
        subscriptions.push_back(bus.subscribe<EnclosureRefundable>([this](const EnclosureRefundable& e) { events.push_back("refundable " + std::to_string(e.slot)); }));
        subscriptions.push_back(bus.subscribe<ComposeChanged>([this](const auto&) { events.push_back("compose"); }));
    }

    void atMailbox() {
        service.openMailbox(7);
        gateway.sent.clear();
        events.clear();
    }
};

} // namespace

TEST_CASE("Opening a mailbox clears the new-mail flag; the server's show keeps the letter", "[mail]") {
    Mailbox box;
    box.newMail(true);
    std::vector<MailChange> changes;
    box.enclose(item(1), kMaxEnclosures, changes);
    auto shown = box.shownByServer(7);
    CHECK(box.hasNewMail());
    CHECK(box.enclosureCount() == 1);
    auto opened = box.opened(7);
    CHECK_FALSE(box.hasNewMail());
    CHECK(has<PendingMailChanged>(opened));
    CHECK(box.enclosureCount() == 0);
}

TEST_CASE("New mail is announced once while it waits", "[mail]") {
    Mailbox box;
    CHECK(has<NewMailArrived>(box.newMail(true)));
    auto again = box.newMail(true);
    CHECK_FALSE(has<NewMailArrived>(again));
    CHECK(has<PendingMailChanged>(again));
}

TEST_CASE("Reading the last unread letter clears the flag without announcing", "[mail]") {
    Mailbox box;
    box.inboxReceived({letter(1), letter(2, true)});
    box.newMail(true);
    auto changes = box.markedRead(1);
    CHECK(has<InboxChanged>(changes));
    CHECK(has<PendingMailChanged>(changes));
    CHECK_FALSE(has<NewMailArrived>(changes));
    CHECK_FALSE(box.hasNewMail());
    CHECK(box.markedRead(1).empty());
}

TEST_CASE("A deleted letter leaves, and the selection follows it", "[mail]") {
    Mailbox box;
    box.inboxReceived({letter(1), letter(2), letter(3)});
    box.select(2);
    box.sendResult(1, MailAction::Deleted, 0);
    CHECK(box.inbox().size() == 2);
    CHECK(box.selected() == 1);
    box.sendResult(3, MailAction::ReturnedToSender, 0);
    CHECK(box.selected() == -1);
    box.sendResult(2, MailAction::Deleted, 9);
    CHECK(box.inbox().size() == 1);
}

TEST_CASE("Taking the money empties the letter's purse", "[mail]") {
    Mailbox box;
    MailMessage m = letter(1);
    m.money = 500;
    box.inboxReceived({m});
    box.sendResult(1, MailAction::MoneyTaken, 0);
    CHECK(box.inbox()[0].money == 0);
}

TEST_CASE("A sent letter empties the enclosures, a refused one keeps them", "[mail]") {
    Mailbox box;
    std::vector<MailChange> changes;
    box.enclose(item(1), kMaxEnclosures, changes);
    CHECK(has<SendFailed>(box.sendResult(0, MailAction::Send, 3)));
    CHECK(box.enclosureCount() == 1);
    const auto sent = box.sendResult(0, MailAction::Send, 0);
    // The line, the emptied letter, then the frame told to reset - in that
    // order, as the interface has always heard them.
    REQUIRE(sent.size() == 3);
    CHECK(std::holds_alternative<MailSent>(sent[0]));
    CHECK(std::holds_alternative<ComposeChanged>(sent[1]));
    CHECK(std::holds_alternative<SendFrameReset>(sent[2]));
    CHECK(box.enclosureCount() == 0);
}

TEST_CASE("A Vanilla letter carries one item", "[mail]") {
    Mailbox box;
    std::vector<MailChange> changes;
    CHECK(box.enclose(item(1), 1, changes) == 0);
    CHECK_FALSE(box.enclose(item(2), 1, changes));
}

TEST_CASE("A late name fills player letters only", "[mail]") {
    Mailbox box;
    MailMessage player = letter(1);
    player.senderGuid = 9;
    MailMessage npc = letter(2, false, 3);
    npc.senderGuid = 9;
    box.inboxReceived({player, npc});
    box.senderNamed(9, "Thrall");
    CHECK(box.inbox()[0].senderName == "Thrall");
    CHECK(box.inbox()[1].senderName.empty());
}

TEST_CASE("An item's arrival redraws the auction letters that name it", "[mail]") {
    Mailbox box;
    MailMessage auction = letter(1, false, 2);
    auction.subject = "1234:0:2:7:1";
    box.inboxReceived({auction, letter(2)});
    CHECK(has<InboxChanged>(box.itemInfoArrived(1234)));
    CHECK(box.itemInfoArrived(99).empty());
}

TEST_CASE("A letter is refused before it leaves, and says why", "[mail]") {
    Fixture f;
    f.service.sendMail("Thrall", "hi", "", 0);
    CHECK(f.events == std::vector<std::string>{"refused: You are not at a mailbox."});
    f.atMailbox();
    f.service.sendMail("", "hi", "", 0);
    CHECK(f.events == std::vector<std::string>{"refused: Enter a recipient."});
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("A letter goes with its enclosures, which are then emptied", "[mail]") {
    Fixture f;
    f.atMailbox();
    f.service.attachItemFromBackpack(0);
    f.service.attachItemFromBackpack(1);
    f.service.sendMail("Thrall", "hi", "", 0);
    CHECK(f.gateway.sent == std::vector<std::string>{"letter Thrall items 2"});
    CHECK(f.service.getMailAttachmentCount() == 0);
}

TEST_CASE("Deleting closes the letter on screen", "[mail]") {
    Fixture f;
    f.atMailbox();
    f.service.mailDelete(4);
    f.service.mailReturnToSender(5);
    CHECK(f.gateway.sent == std::vector<std::string>{"delete 4", "return 5"});
    CHECK(f.events == std::vector<std::string>{"closed 4", "closed 5"});
}

TEST_CASE("An item still within its refund window is flagged as it is enclosed", "[mail]") {
    Fixture f;
    f.atMailbox();
    f.gateway.refunds[100] = IMailWorld::Refund::Open;
    f.gateway.refunds[101] = IMailWorld::Refund::Over;
    f.service.attachItemFromBackpack(0);
    f.service.attachItemFromBackpack(1);
    f.service.attachItemFromBackpack(2);      // not known yet: nothing said
    CHECK(f.events == std::vector<std::string>{"refundable 0", "compose", "compose", "compose"});
}

TEST_CASE("An arriving inbox asks for unknown senders and auctioned items", "[mail]") {
    Fixture f;
    f.atMailbox();
    f.gateway.players[8] = "Jaina";
    MailMessage unknown = letter(1);
    unknown.senderGuid = 9;
    MailMessage known = letter(2);
    known.senderGuid = 8;
    MailMessage auction = letter(3, false, 2);
    auction.subject = "1234:0:1";
    MailAttachment att;
    att.itemId = 55;
    auction.attachments.push_back(att);
    f.service.onInbox({unknown, known, auction});
    CHECK(f.gateway.asked == std::vector<std::string>{"name 9", "item 1234", "item 55"});
}

TEST_CASE("Senders and auction subjects are shown in words", "[mail]") {
    Fixture f;
    f.gateway.players[8] = "Jaina";
    MailMessage player = letter(1);
    player.senderGuid = 8;
    CHECK(f.service.getMailSenderName(player) == "Jaina");
    player.senderGuid = 9;
    CHECK(f.service.getMailSenderName(player) == "Unknown");
    MailMessage auction = letter(2, false, 2);
    auction.subject = "1234:0:1";
    CHECK(f.service.getMailSenderName(auction) == "Auction House");
    CHECK(f.service.getMailDisplaySubject(auction) == "Auction won: Item #1234");
    f.gateway.items[1234] = "Thunderfury";
    CHECK(f.service.getMailDisplaySubject(auction) == "Auction won: Thunderfury");
}

TEST_CASE("The next character is at no mailbox and has none of the last one's mail", "[mail]") {
    Fixture f;
    f.atMailbox();
    f.service.onInbox({letter(1, false)});
    f.service.onNewMail(true);
    REQUIRE(f.service.attachItemFromBackpack(0));

    f.service.beginCharacter();

    CHECK_FALSE(f.service.isMailboxOpen());
    CHECK(f.service.getMailInbox().empty());
    CHECK_FALSE(f.service.hasNewMail());
    CHECK(f.service.getMailAttachmentCount() == 0);
    // No mailbox, so nothing goes out.
    f.service.mailDelete(1);
    CHECK(f.gateway.sent.empty());
}
