#pragma once

#include <cstdint>
#include <string>

namespace wowee::contacts {

/// Requests to the server about the contact lists. Implemented by the
/// protocol layer, which knows the expansion's opcodes and the socket.
class IContactsGateway {
public:
    virtual ~IContactsGateway() = default;

    /// In the world with a connection; requests are dropped otherwise.
    [[nodiscard]] virtual bool canSend() const = 0;

    virtual void sendAddFriend(const std::string& name, const std::string& note) = 0;
    virtual void sendRemoveFriend(uint64_t guid) = 0;
    virtual void sendFriendNote(uint64_t guid, const std::string& note) = 0;
    virtual void sendAddIgnore(const std::string& name) = 0;
    virtual void sendRemoveIgnore(uint64_t guid) = 0;
    virtual void sendContactListRequest() = 0;
};

/// The client's cache of player names, and the query that fills it.
class IPlayerNames {
public:
    virtual ~IPlayerNames() = default;

    /// Empty when the name has not been resolved yet.
    [[nodiscard]] virtual std::string cachedName(uint64_t guid) const = 0;
    virtual void requestName(uint64_t guid) = 0;
};

} // namespace wowee::contacts
