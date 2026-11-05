// The contacts service against a recording gateway and the real event bus.
#include <catch_amalgamated.hpp>

#include "contacts/services/contacts_service.hpp"
#include "foundation/event_bus.hpp"

#include <map>
#include <string>
#include <vector>

using namespace wowee::contacts;

namespace {

struct RecordingGateway final : IContactsGateway {
    bool inWorld = true;
    std::vector<std::string> sent;

    bool canSend() const override { return inWorld; }
    void sendAddFriend(const std::string& name, const std::string&) override { sent.push_back("add " + name); }
    void sendRemoveFriend(uint64_t guid) override { sent.push_back("remove " + std::to_string(guid)); }
    void sendFriendNote(uint64_t guid, const std::string& note) override { sent.push_back("note " + std::to_string(guid) + " " + note); }
    void sendAddIgnore(const std::string& name) override { sent.push_back("ignore " + name); }
    void sendRemoveIgnore(uint64_t guid) override { sent.push_back("unignore " + std::to_string(guid)); }
    void sendContactListRequest() override { sent.push_back("list"); }
};

struct NameCache final : IPlayerNames {
    std::map<uint64_t, std::string> names;
    std::vector<uint64_t> queried;

    std::string cachedName(uint64_t guid) const override {
        auto it = names.find(guid);
        return it != names.end() ? it->second : std::string();
    }
    void requestName(uint64_t guid) override { queried.push_back(guid); }
};

struct Fixture {
    RecordingGateway gateway;
    NameCache names;
    wowee::core::EventBus bus;
    ContactsService service{gateway, names, bus};
    std::vector<std::string> notices;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<NotAFriend>([this](const NotAFriend& e) { notices.push_back("not a friend: " + e.name); }));
        subscriptions.push_back(bus.subscribe<NotIgnored>([this](const NotIgnored& e) { notices.push_back("not ignored: " + e.name); }));
        subscriptions.push_back(bus.subscribe<ContactNameRequired>([this](const auto&) { notices.push_back("name required"); }));
        subscriptions.push_back(bus.subscribe<FriendRemovalRequested>([this](const FriendRemovalRequested& e) { notices.push_back("removing " + e.name); }));
        subscriptions.push_back(bus.subscribe<FriendCameOnline>([this](const FriendCameOnline& e) { notices.push_back("online " + e.name); }));
    }

    void friendOnList(uint64_t guid, const std::string& name) {
        names.names[guid] = name;
        ContactRecord r;
        r.guid = guid;
        r.flags = ContactFlag::Friend;
        service.onContactList({r}, ContactFlag::Friend);
    }
};

} // namespace

TEST_CASE("Removing a friend sends the guid the list holds for the name", "[contacts]") {
    Fixture f;
    f.friendOnList(42, "Thrall");
    f.service.removeFriend("Thrall");
    CHECK(f.gateway.sent == std::vector<std::string>{"remove 42"});
    CHECK(f.notices == std::vector<std::string>{"removing Thrall"});
    // The row stays until the server confirms.
    CHECK(f.service.contacts().size() == 1);
}

TEST_CASE("Removing someone who is not a friend sends nothing", "[contacts]") {
    Fixture f;
    f.service.removeFriend("Thrall");
    f.service.setFriendNote("Thrall", "tank");
    f.service.removeIgnore("Garrosh");
    CHECK(f.gateway.sent.empty());
    CHECK(f.notices == std::vector<std::string>{"not a friend: Thrall", "not a friend: Thrall",
                                                "not ignored: Garrosh"});
}

TEST_CASE("A request without a name is refused", "[contacts]") {
    Fixture f;
    f.service.addFriend("");
    f.service.addIgnore("");
    CHECK(f.gateway.sent.empty());
    CHECK(f.notices == std::vector<std::string>{"name required", "name required"});
}

TEST_CASE("Outside the world requests are dropped silently", "[contacts]") {
    Fixture f;
    f.friendOnList(42, "Thrall");
    f.gateway.inWorld = false;
    f.service.addFriend("Jaina");
    f.service.removeFriend("Thrall");
    f.service.refresh();
    CHECK(f.gateway.sent.empty());
    CHECK(f.notices.empty());
}

TEST_CASE("Friends whose names are unknown are looked up", "[contacts]") {
    Fixture f;
    ContactRecord r;
    r.guid = 99;
    f.service.onFriendList({r});
    CHECK(f.names.queried == std::vector<uint64_t>{99});

    f.service.onPlayerNameResolved(99, "Jaina");
    f.service.removeFriend("Jaina");
    CHECK(f.gateway.sent == std::vector<std::string>{"remove 99"});
}

TEST_CASE("A friend coming online is announced by the name on the list", "[contacts]") {
    Fixture f;
    f.friendOnList(42, "Thrall");
    f.names.names.clear();          // the cache no longer has him
    FriendStatusUpdate u;
    u.result = static_cast<uint8_t>(FriendResult::Online);
    u.guid = 42;
    u.chatFlag = 0x01;
    f.service.onFriendStatus(u);
    CHECK(f.notices == std::vector<std::string>{"online Thrall"});
}

TEST_CASE("A character logging in does not inherit the last one's lists", "[contacts]") {
    Fixture f;
    f.friendOnList(42, "Thrall");
    f.service.beginCharacter();
    f.service.removeFriend("Thrall");
    CHECK(f.gateway.sent.empty());
}
