// The friend, ignore and mute lists, end to end: the server's packets, the
// lists the interface reads, what it is told, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "contacts/services/contacts.hpp"
#include "game/game_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kThrall = 0x20;
constexpr uint64_t kJaina = 0x21;
constexpr uint32_t kFriend = 0x1, kIgnored = 0x2;

/// SMSG_NAME_QUERY_RESPONSE for a player who exists.
std::vector<uint8_t> nameResponse(uint64_t guid, const std::string& name) {
    return Body().packedGuid(guid).u8(0 /* found */).cstr(name).cstr("").u8(2).u8(0).u8(7);
}

struct Row {
    uint64_t guid;
    uint32_t flags;
    std::string note;
    uint8_t status = 0;    // friends only; nonzero brings area, level and class
};

/// SMSG_CONTACT_LIST as AzerothCore's SocialMgr::SendSocialList writes it.
std::vector<uint8_t> contactList(uint32_t mask, const std::vector<Row>& rows) {
    Body b;
    b.u32(mask).u32(static_cast<uint32_t>(rows.size()));
    for (const auto& r : rows) {
        b.u64(r.guid).u32(r.flags).cstr(r.note);
        if (r.flags & kFriend) {
            b.u8(r.status);
            if (r.status != 0) b.u32(1519 /* Stormwind */).u32(80).u32(7 /* shaman */);
        }
    }
    return b.bytes();
}

const wowee::contacts::Contact* row(GameHarness& h, uint64_t guid) {
    for (const auto& c : h.contacts().contacts())
        if (c.guid == guid) return &c;
    return nullptr;
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

}  // namespace

TEST_CASE("the contact list the server sends is what the friends panel reads", "[contacts][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_CONTACT_LIST,
              contactList(kFriend | kIgnored, {{kThrall, kFriend, "warchief", 0x03 /* online, away */},
                                               {kJaina, kIgnored, ""}}));
    h.pump();

    const auto* thrall = row(h, kThrall);
    REQUIRE(thrall);
    CHECK(thrall->isFriend());
    CHECK(thrall->isOnline());
    CHECK(thrall->isAway());
    CHECK(thrall->note == "warchief");
    CHECK(thrall->level == 80);
    const auto* jaina = row(h, kJaina);
    REQUIRE(jaina);
    CHECK(jaina->isIgnored());
    CHECK_FALSE(jaina->isFriend());
    // A list sent in reply to a request: shown as well as changed, and the
    // ignore tab told too because the mask says it came.
    CHECK(h.addonEvents("FRIENDLIST_UPDATE").size() == 1);
    CHECK(h.addonEvents("FRIENDLIST_SHOW").size() == 1);
    CHECK(h.addonEvents("IGNORELIST_UPDATE").size() == 1);
}

TEST_CASE("a friend whose name comes after the list is removed by that name", "[contacts][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_CONTACT_LIST, contactList(kFriend, {{kThrall, kFriend, ""}}));
    h.pump();
    // The list carries guids only, so the game asks who this is.
    REQUIRE(h.lastSent(Opcode::CMSG_NAME_QUERY));

    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, nameResponse(kThrall, "Thrall"));
    h.pump();
    h.contacts().removeFriend("Thrall");

    auto* sent = h.lastSent(Opcode::CMSG_DEL_FRIEND);
    REQUIRE(sent);
    CHECK(sent->readUInt64() == kThrall);
    CHECK(chatSaid(h, "Removing Thrall from friends list..."));
}

TEST_CASE("removing a name that is not a friend asks the server nothing", "[contacts][harness]") {
    GameHarness h;
    h.enterWorld();

    h.contacts().removeFriend("Nobody");
    h.pump();

    CHECK_FALSE(h.lastSent(Opcode::CMSG_DEL_FRIEND));
    CHECK(chatSaid(h, "Nobody is not in your friends list."));
}

TEST_CASE("a friend added online and later removed comes and goes with its chat lines", "[contacts][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, nameResponse(kThrall, "Thrall"));
    h.pump();

    // FRIEND_ADDED_ONLINE: the note, then status, area, level and class.
    h.receive(Opcode::SMSG_FRIEND_STATUS,
              Body().u8(6).u64(kThrall).cstr("").u8(0x01).u32(1519).u32(80).u32(7).bytes());
    h.pump();
    REQUIRE(row(h, kThrall));
    CHECK(row(h, kThrall)->isOnline());
    CHECK(chatSaid(h, "Thrall has been added to your friends list."));

    // FRIEND_REMOVED.
    h.receive(Opcode::SMSG_FRIEND_STATUS, Body().u8(5).u64(kThrall).bytes());
    h.pump();
    CHECK_FALSE(row(h, kThrall));
    CHECK(chatSaid(h, "Thrall has been removed from your friends list."));
    CHECK(h.addonEvents("FRIENDLIST_UPDATE").size() == 2);
}

TEST_CASE("another character logging in does not keep the last one's friends", "[contacts][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_NAME_QUERY_RESPONSE, nameResponse(kThrall, "Thrall"));
    h.receive(Opcode::SMSG_CONTACT_LIST, contactList(kFriend, {{kThrall, kFriend, ""}}));
    h.pump();
    REQUIRE(row(h, kThrall));

    PlayerSpec next;
    next.guid = 0x11;
    next.name = "Second";
    h.relogAs(next);

    CHECK(h.contacts().contacts().empty());
    h.contacts().removeFriend("Thrall");
    CHECK_FALSE(h.lastSent(Opcode::CMSG_DEL_FRIEND));
}
