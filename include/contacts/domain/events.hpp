#pragma once

#include <string>
#include <variant>

namespace wowee::contacts {

// What happened to the contact lists. ContactList returns these; the service
// publishes them, and the chat log and the addon layer subscribe.

/// The friend list's rows changed (FRIENDLIST_UPDATE).
struct FriendListChanged {};
/// A whole friend list arrived in reply to a request (FRIENDLIST_SHOW).
struct FriendListShown {};
/// The ignore list changed (IGNORELIST_UPDATE).
struct IgnoreListChanged {};

// Outcomes the server reported for one friend.
struct FriendListUnavailable {};
struct FriendListFull {};
struct FriendCameOnline  { std::string name; };
struct FriendWentOffline { std::string name; };
struct FriendNotFound {};
struct FriendRemoved     { std::string name; };
struct FriendAdded       { std::string name; };
struct AlreadyFriend     { std::string name; };
struct CannotBefriendSelf {};
struct FriendIsHostile   { std::string name; };

using ContactEvent = std::variant<
    FriendListChanged, FriendListShown, IgnoreListChanged,
    FriendListUnavailable, FriendListFull, FriendCameOnline, FriendWentOffline,
    FriendNotFound, FriendRemoved, FriendAdded, AlreadyFriend,
    CannotBefriendSelf, FriendIsHostile>;

} // namespace wowee::contacts
