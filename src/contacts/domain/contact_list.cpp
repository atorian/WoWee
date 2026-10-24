#include "contacts/domain/contact_list.hpp"

#include <algorithm>

namespace wowee::contacts {

namespace {

bool usableName(std::string_view name) {
    return !name.empty() && name != "Unknown";
}

void assignRecord(Contact& c, ContactRecord&& r) {
    c.note    = std::move(r.note);
    c.status  = r.status;
    c.areaId  = r.areaId;
    c.level   = r.level;
    c.classId = r.classId;
    if (!r.name.empty()) c.name = std::move(r.name);
}

} // namespace

Contact& ContactList::upsert(uint64_t guid) {
    auto it = std::find_if(contacts_.begin(), contacts_.end(),
                           [&](const Contact& c) { return c.guid == guid; });
    if (it != contacts_.end()) return *it;
    Contact& c = contacts_.emplace_back();
    c.guid = guid;
    return c;
}

void ContactList::dropFlag(uint32_t flag) {
    for (auto& c : contacts_) c.flags &= ~flag;
}

void ContactList::eraseEmpty() {
    contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(),
                                   [](const Contact& c) { return c.flags == 0; }),
                    contacts_.end());
}

std::vector<ContactEvent> ContactList::replaceAll(std::vector<ContactRecord> records,
                                                  uint32_t listMask) {
    contacts_.clear();
    for (auto& r : records) {
        Contact& c = upsert(r.guid);
        c.flags |= r.flags;
        assignRecord(c, std::move(r));
    }
    // The whole list arrives in reply to this client asking for it, so it is
    // the "show" case as well as the "changed" one.
    std::vector<ContactEvent> events{FriendListChanged{}, FriendListShown{}};
    if (listMask & ContactFlag::Ignored) events.emplace_back(IgnoreListChanged{});
    return events;
}

std::vector<ContactEvent> ContactList::replaceFriends(std::vector<ContactRecord> records) {
    dropFlag(ContactFlag::Friend);
    eraseEmpty();
    for (auto& r : records) {
        Contact& c = upsert(r.guid);
        c.flags |= ContactFlag::Friend;
        assignRecord(c, std::move(r));
    }
    return {FriendListChanged{}, FriendListShown{}};
}

std::vector<ContactEvent> ContactList::replaceIgnored(std::vector<ContactRecord> records) {
    dropFlag(ContactFlag::Ignored);
    eraseEmpty();
    for (auto& r : records) {
        Contact& c = upsert(r.guid);
        c.flags |= ContactFlag::Ignored;
        if (!r.name.empty()) c.name = std::move(r.name);
    }
    return {IgnoreListChanged{}};
}

std::vector<ContactEvent> ContactList::applyFriendStatus(const FriendStatusUpdate& u,
                                                         std::string_view knownName) {
    auto it = std::find_if(contacts_.begin(), contacts_.end(),
                           [&](const Contact& c) { return c.guid == u.guid; });
    const bool known = it != contacts_.end();
    std::string name = (known && !it->name.empty()) ? it->name : std::string(knownName);

    const auto result = static_cast<FriendResult>(u.result);
    switch (result) {
        case FriendResult::Removed:
            if (known) {
                it->flags &= ~ContactFlag::Friend;
                eraseEmpty();
            }
            break;
        case FriendResult::Online:
        case FriendResult::Offline:
        case FriendResult::AddedOnline:
        case FriendResult::AddedOffline: {
            const bool online = result == FriendResult::Online ||
                                result == FriendResult::AddedOnline;
            const bool added = result == FriendResult::AddedOnline ||
                               result == FriendResult::AddedOffline;
            // chatFlag is the FriendStatus bitmask; offline results carry no
            // body, so the flags are cleared rather than left as they were.
            const uint8_t status = online ? u.chatFlag : 0;
            if (known) {
                if (usableName(name)) it->name = name;
                if (added) it->flags |= ContactFlag::Friend;
                it->status = status;
                if (online) {
                    it->areaId  = u.areaId;
                    it->level   = u.level;
                    it->classId = u.classId;
                }
                if (!u.note.empty()) it->note = u.note;
            } else {
                Contact& c = contacts_.emplace_back();
                c.guid    = u.guid;
                c.name    = name;
                c.flags   = ContactFlag::Friend;
                c.status  = status;
                c.note    = u.note;
                c.areaId  = u.areaId;
                c.level   = u.level;
                c.classId = u.classId;
            }
            break;
        }
        default:
            break;
    }

    std::vector<ContactEvent> events;
    switch (result) {
        case FriendResult::DbError:      events.emplace_back(FriendListUnavailable{}); break;
        case FriendResult::ListFull:     events.emplace_back(FriendListFull{}); break;
        case FriendResult::Online:       events.emplace_back(FriendCameOnline{name}); break;
        case FriendResult::Offline:      events.emplace_back(FriendWentOffline{name}); break;
        case FriendResult::NotFound:     events.emplace_back(FriendNotFound{}); break;
        case FriendResult::Removed:      events.emplace_back(FriendRemoved{name}); break;
        case FriendResult::AddedOnline:
        case FriendResult::AddedOffline: events.emplace_back(FriendAdded{name}); break;
        case FriendResult::Already:      events.emplace_back(AlreadyFriend{name}); break;
        case FriendResult::Self:         events.emplace_back(CannotBefriendSelf{}); break;
        case FriendResult::Enemy:        events.emplace_back(FriendIsHostile{name}); break;
        default: break;
    }
    events.emplace_back(FriendListChanged{});
    return events;
}

void ContactList::nameResolved(uint64_t guid, std::string_view name) {
    if (!usableName(name)) return;
    for (auto& c : contacts_)
        if (c.guid == guid) c.name = std::string(name);
}

void ContactList::clear() {
    contacts_.clear();
}

const Contact* ContactList::find(uint64_t guid) const {
    for (const auto& c : contacts_)
        if (c.guid == guid) return &c;
    return nullptr;
}

const Contact* ContactList::findFriend(std::string_view name) const {
    for (const auto& c : contacts_)
        if (c.isFriend() && c.name == name) return &c;
    return nullptr;
}

const Contact* ContactList::findIgnored(std::string_view name) const {
    for (const auto& c : contacts_)
        if (c.isIgnored() && c.name == name) return &c;
    return nullptr;
}

} // namespace wowee::contacts
