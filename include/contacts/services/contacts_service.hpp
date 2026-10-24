#pragma once

#include "contacts/domain/contact_list.hpp"
#include "contacts/services/contacts.hpp"
#include "contacts/services/ports.hpp"

#include <string>

namespace wowee::core { class EventBus; }

namespace wowee::contacts {

// What became of a request the player made. Published like the domain events,
// so the chat log reports them whichever way the request was made - a slash
// command, the friends panel or a nameplate menu.
struct FriendRequestSent      { std::string name; };
struct FriendRemovalRequested { std::string name; };
struct FriendNoteSent         { std::string name; };
struct IgnoreRequested        { std::string name; };
struct IgnoreRemovalRequested { std::string name; };
/// Rejected: no name was given.
struct ContactNameRequired {};
/// Rejected: the name is not on the friend list.
struct NotAFriend             { std::string name; };
/// Rejected: the name is not on the ignore list.
struct NotIgnored             { std::string name; };

class ContactsService final : public IContacts, public IContactsServerFeed {
public:
    ContactsService(IContactsGateway& gateway, IPlayerNames& names, core::EventBus& events);

    // IContacts
    [[nodiscard]] const std::vector<Contact>& contacts() const override { return list_.contacts(); }
    void addFriend(const std::string& name, const std::string& note = "") override;
    void removeFriend(const std::string& name) override;
    void setFriendNote(const std::string& name, const std::string& note) override;
    void addIgnore(const std::string& name) override;
    void removeIgnore(const std::string& name) override;
    void refresh() override;

    // IContactsServerFeed
    void onContactList(std::vector<ContactRecord> records, uint32_t listMask) override;
    void onFriendList(std::vector<ContactRecord> records) override;
    void onIgnoreList(const std::vector<uint64_t>& guids) override;
    void onFriendStatus(const FriendStatusUpdate& update) override;
    void onPlayerNameResolved(uint64_t guid, const std::string& name) override;
    void beginCharacter() override;

private:
    void resolveNames(std::vector<ContactRecord>& records, bool queryUnknown);
    void publish(const std::vector<ContactEvent>& events);

    IContactsGateway& gateway_;
    IPlayerNames& names_;
    core::EventBus& events_;
    ContactList list_;
};

} // namespace wowee::contacts
