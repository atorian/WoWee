#pragma once

#include "contacts/domain/contact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::contacts {

/// The contact lists as the interface and the addon layer use them: read the
/// lists, and ask the server to change them. The server's answer arrives as
/// events, not as a return value.
class IContacts {
public:
    virtual ~IContacts() = default;

    [[nodiscard]] virtual const std::vector<Contact>& contacts() const = 0;

    virtual void addFriend(const std::string& name, const std::string& note = "") = 0;
    virtual void removeFriend(const std::string& name) = 0;
    virtual void setFriendNote(const std::string& name, const std::string& note) = 0;
    virtual void addIgnore(const std::string& name) = 0;
    virtual void removeIgnore(const std::string& name) = 0;
    /// Ask the server to resend every list. FriendsFrame calls this on each
    /// redraw, so it is cheap to call often.
    virtual void refresh() = 0;
};

/// What the protocol layer tells the contact lists, already decoded from the
/// packets of whichever expansion is connected.
class IContactsServerFeed {
public:
    virtual ~IContactsServerFeed() = default;

    virtual void onContactList(std::vector<ContactRecord> records, uint32_t listMask) = 0;
    virtual void onFriendList(std::vector<ContactRecord> records) = 0;
    virtual void onIgnoreList(const std::vector<uint64_t>& guids) = 0;
    virtual void onFriendStatus(const FriendStatusUpdate& update) = 0;
    virtual void onPlayerNameResolved(uint64_t guid, const std::string& name) = 0;
    /// A character is logging in; the last one's lists are not its own.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::contacts
