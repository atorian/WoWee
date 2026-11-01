#include "contacts/services/contacts_service.hpp"

#include "core/event_bus.hpp"

namespace wowee::contacts {

ContactsService::ContactsService(IContactsGateway& gateway, IPlayerNames& names,
                                 core::EventBus& events)
    : gateway_(gateway), names_(names), events_(events) {}

void ContactsService::addFriend(const std::string& name, const std::string& note) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(ContactNameRequired{}); return; }
    gateway_.sendAddFriend(name, note);
    events_.publish(FriendRequestSent{name});
}

void ContactsService::removeFriend(const std::string& name) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(ContactNameRequired{}); return; }
    const Contact* c = list_.findFriend(name);
    if (!c) { events_.publish(NotAFriend{name}); return; }
    // The row stays until SMSG_FRIEND_STATUS confirms the removal.
    gateway_.sendRemoveFriend(c->guid);
    events_.publish(FriendRemovalRequested{name});
}

void ContactsService::setFriendNote(const std::string& name, const std::string& note) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(ContactNameRequired{}); return; }
    const Contact* c = list_.findFriend(name);
    if (!c) { events_.publish(NotAFriend{name}); return; }
    gateway_.sendFriendNote(c->guid, note);
    events_.publish(FriendNoteSent{name});
}

void ContactsService::addIgnore(const std::string& name) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(ContactNameRequired{}); return; }
    gateway_.sendAddIgnore(name);
    events_.publish(IgnoreRequested{name});
}

void ContactsService::removeIgnore(const std::string& name) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(ContactNameRequired{}); return; }
    const Contact* c = list_.findIgnored(name);
    if (!c) { events_.publish(NotIgnored{name}); return; }
    // The row stays until the server sends the list again: removing it here
    // would desync the list if the server refused.
    gateway_.sendRemoveIgnore(c->guid);
    events_.publish(IgnoreRemovalRequested{name});
}

void ContactsService::refresh() {
    if (!gateway_.canSend()) return;
    gateway_.sendContactListRequest();
}

void ContactsService::resolveNames(std::vector<ContactRecord>& records, bool onlyFriends) {
    for (auto& r : records) {
        r.name = names_.cachedName(r.guid);
        const bool wanted = !onlyFriends || (r.flags & ContactFlag::Friend);
        if (r.name.empty() && wanted && r.guid != 0) names_.requestName(r.guid);
    }
}

void ContactsService::onContactList(std::vector<ContactRecord> records, uint32_t listMask) {
    resolveNames(records, true);
    publish(list_.replaceAll(std::move(records), listMask));
}

void ContactsService::onFriendList(std::vector<ContactRecord> records) {
    resolveNames(records, false);
    publish(list_.replaceFriends(std::move(records)));
}

void ContactsService::onIgnoreList(const std::vector<uint64_t>& guids) {
    std::vector<ContactRecord> records;
    records.reserve(guids.size());
    for (uint64_t guid : guids) {
        if (guid == 0) continue;
        ContactRecord r;
        r.guid = guid;
        r.flags = ContactFlag::Ignored;
        records.push_back(std::move(r));
    }
    resolveNames(records, false);
    publish(list_.replaceIgnored(std::move(records)));
}

void ContactsService::onFriendStatus(const FriendStatusUpdate& update) {
    publish(list_.applyFriendStatus(update, names_.cachedName(update.guid)));
}

void ContactsService::onPlayerNameResolved(uint64_t guid, const std::string& name) {
    list_.nameResolved(guid, name);
}

void ContactsService::beginCharacter() {
    list_.clear();
}

void ContactsService::publish(const std::vector<ContactEvent>& events) {
    for (const auto& e : events)
        std::visit([this](const auto& ev) { events_.publish(ev); }, e);
}

} // namespace wowee::contacts
