#pragma once

#include "contacts/domain/contact.hpp"
#include "contacts/domain/events.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace wowee::contacts {

/// The friend, ignore and mute lists of the character in the world.
///
/// One row per player, whichever lists they are on. Every change comes from
/// the server - a whole list, or the outcome for one friend - and returns
/// what happened, for the service to publish.
class ContactList {
public:
    /// SMSG_CONTACT_LIST (TBC and WotLK): every list at once, replacing all
    /// of them. `listMask` says which lists the server sent.
    std::vector<ContactEvent> replaceAll(std::vector<ContactRecord> records, uint32_t listMask);

    /// SMSG_FRIEND_LIST (Vanilla): the friend list alone. Ignored and muted
    /// players stay.
    std::vector<ContactEvent> replaceFriends(std::vector<ContactRecord> records);

    /// SMSG_IGNORE_LIST (Vanilla): the ignore list alone. Friends stay.
    std::vector<ContactEvent> replaceIgnored(std::vector<ContactRecord> records);

    /// SMSG_FRIEND_STATUS. `knownName` is the name cache's answer for the
    /// guid; the list's own name for the friend wins over it.
    std::vector<ContactEvent> applyFriendStatus(const FriendStatusUpdate& update,
                                                std::string_view knownName);

    /// A name query answered for a player who may be on a list.
    void nameResolved(uint64_t guid, std::string_view name);

    /// A character is logging in: it has lists of its own, which the server
    /// sends with the login.
    void clear();

    [[nodiscard]] const std::vector<Contact>& contacts() const { return contacts_; }
    [[nodiscard]] const Contact* find(uint64_t guid) const;
    [[nodiscard]] const Contact* findFriend(std::string_view name) const;
    [[nodiscard]] const Contact* findIgnored(std::string_view name) const;

private:
    Contact& upsert(uint64_t guid);
    void dropFlag(uint32_t flag);
    void eraseEmpty();

    std::vector<Contact> contacts_;
};

} // namespace wowee::contacts
