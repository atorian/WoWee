// The contact lists' rules, with nothing but the domain model linked.
#include <catch_amalgamated.hpp>

#include "contacts/domain/contact_list.hpp"

#include <algorithm>
#include <variant>

using namespace wowee::contacts;

namespace {

ContactRecord row(uint64_t guid, uint32_t flags, std::string name = "") {
    ContactRecord r;
    r.guid = guid;
    r.flags = flags;
    r.name = std::move(name);
    return r;
}

FriendStatusUpdate status(FriendResult result, uint64_t guid, uint8_t chatFlag = 0) {
    FriendStatusUpdate u;
    u.result = static_cast<uint8_t>(result);
    u.guid = guid;
    u.chatFlag = chatFlag;
    return u;
}

template <typename E>
bool has(const std::vector<ContactEvent>& events) {
    return std::any_of(events.begin(), events.end(),
                       [](const ContactEvent& e) { return std::holds_alternative<E>(e); });
}

template <typename E>
const E& first(const std::vector<ContactEvent>& events) {
    for (const auto& e : events)
        if (auto* p = std::get_if<E>(&e)) return *p;
    FAIL("event not published");
    throw;
}

} // namespace

TEST_CASE("An away friend is online and away, not busy", "[contacts]") {
    // FriendStatus is a bitmask: away is 0x03. Read as a number it was shown
    // as do-not-disturb.
    ContactList list;
    list.applyFriendStatus(status(FriendResult::AddedOnline, 7, 0x03), "Thrall");
    const Contact* c = list.find(7);
    REQUIRE(c);
    CHECK(c->isOnline());
    CHECK(c->isAway());
    CHECK_FALSE(c->isBusy());
}

TEST_CASE("A database error leaves the friend on the list", "[contacts]") {
    ContactList list;
    list.replaceAll({row(7, ContactFlag::Friend, "Thrall")}, ContactFlag::Friend);
    auto events = list.applyFriendStatus(status(FriendResult::DbError, 7), "");
    CHECK(list.findFriend("Thrall"));
    CHECK(has<FriendListUnavailable>(events));
    CHECK_FALSE(has<FriendRemoved>(events));
}

TEST_CASE("Result 5 removes the friend and result 7 adds one offline", "[contacts]") {
    ContactList list;
    list.replaceAll({row(7, ContactFlag::Friend, "Thrall")}, ContactFlag::Friend);

    auto removed = list.applyFriendStatus(status(FriendResult::Removed, 7), "");
    CHECK_FALSE(list.find(7));
    CHECK(first<FriendRemoved>(removed).name == "Thrall");

    auto added = list.applyFriendStatus(status(FriendResult::AddedOffline, 9), "Jaina");
    const Contact* c = list.findFriend("Jaina");
    REQUIRE(c);
    CHECK_FALSE(c->isOnline());
    CHECK(first<FriendAdded>(added).name == "Jaina");
}

TEST_CASE("Removing a friend who is also ignored keeps the ignore", "[contacts]") {
    ContactList list;
    list.replaceAll({row(7, ContactFlag::Friend | ContactFlag::Ignored, "Garrosh")},
                    ContactFlag::Friend | ContactFlag::Ignored);
    list.applyFriendStatus(status(FriendResult::Removed, 7), "");
    CHECK_FALSE(list.findFriend("Garrosh"));
    CHECK(list.findIgnored("Garrosh"));
}

TEST_CASE("The list's own name beats an unresolved cache answer", "[contacts]") {
    ContactList list;
    list.replaceAll({row(7, ContactFlag::Friend, "Thrall")}, ContactFlag::Friend);
    auto events = list.applyFriendStatus(status(FriendResult::Online, 7, 0x01), "");
    CHECK(first<FriendCameOnline>(events).name == "Thrall");
    CHECK(list.find(7)->name == "Thrall");
}

TEST_CASE("Going offline clears the away flag", "[contacts]") {
    ContactList list;
    list.applyFriendStatus(status(FriendResult::AddedOnline, 7, 0x03), "Thrall");
    list.applyFriendStatus(status(FriendResult::Offline, 7, 0x03), "Thrall");
    CHECK_FALSE(list.find(7)->isOnline());
    CHECK_FALSE(list.find(7)->isAway());
}

TEST_CASE("A Vanilla friend list leaves the ignore list alone, and back", "[contacts]") {
    ContactList list;
    list.replaceIgnored({row(1, 0, "Garrosh")});
    list.replaceFriends({row(2, 0, "Thrall")});
    CHECK(list.findIgnored("Garrosh"));
    CHECK(list.findFriend("Thrall"));

    list.replaceIgnored({});
    CHECK_FALSE(list.findIgnored("Garrosh"));
    CHECK(list.findFriend("Thrall"));
}

TEST_CASE("A Vanilla ignore list fires IGNORELIST_UPDATE", "[contacts]") {
    ContactList list;
    CHECK(has<IgnoreListChanged>(list.replaceIgnored({row(1, 0)})));
}

TEST_CASE("A name that resolves later fills the row", "[contacts]") {
    // SMSG_IGNORE_LIST carries guids only; /unignore looks players up by name.
    ContactList list;
    list.replaceIgnored({row(1, 0)});
    CHECK_FALSE(list.findIgnored("Garrosh"));
    list.nameResolved(1, "Garrosh");
    CHECK(list.findIgnored("Garrosh"));
    list.nameResolved(1, "Unknown");
    CHECK(list.find(1)->name == "Garrosh");
}

TEST_CASE("The contact list mask decides whether the ignore list changed", "[contacts]") {
    ContactList list;
    auto friendsOnly = list.replaceAll({}, ContactFlag::Friend);
    CHECK(has<FriendListShown>(friendsOnly));
    CHECK_FALSE(has<IgnoreListChanged>(friendsOnly));
    CHECK(has<IgnoreListChanged>(list.replaceAll({}, ContactFlag::Friend | ContactFlag::Ignored)));
}

TEST_CASE("Clearing for a new character empties every list", "[contacts]") {
    ContactList list;
    list.replaceAll({row(7, ContactFlag::Friend, "Thrall")}, ContactFlag::Friend);
    list.clear();
    CHECK(list.contacts().empty());
    CHECK_FALSE(list.findFriend("Thrall"));
}
