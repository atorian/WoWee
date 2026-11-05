#pragma once

#include "contacts/domain/contact.hpp"
#include "contacts/services/ports.hpp"
#include "foundation/event_bus.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class SocialQueries;
class ChatLog;
class FrameXmlEvents;
struct FriendStatusData;

/// The contact service's ports, implemented over the world connection and
/// the client's name cache.
class ContactsGateway final : public contacts::IContactsGateway,
                              public contacts::IPlayerNames {
public:
    ContactsGateway(PacketSender& sender, SocialQueries& names) : sender_(sender), names_(names) {}

    // IContactsGateway
    [[nodiscard]] bool canSend() const override;
    void sendAddFriend(const std::string& name, const std::string& note) override;
    void sendRemoveFriend(uint64_t guid) override;
    void sendFriendNote(uint64_t guid, const std::string& note) override;
    void sendAddIgnore(const std::string& name) override;
    void sendRemoveIgnore(uint64_t guid) override;
    void sendContactListRequest() override;

    // IPlayerNames
    [[nodiscard]] std::string cachedName(uint64_t guid) const override;
    void requestName(uint64_t guid) override;

private:
    PacketSender& sender_;
    SocialQueries& names_;
    std::chrono::steady_clock::time_point lastListRequest_{};
};

// Decoding the contact packets. Each returns nothing when the packet is too
// short to hold even its header, which leaves the lists as they were.

/// SMSG_CONTACT_LIST (TBC, WotLK): uint32 list mask, uint32 count, rows.
std::optional<std::vector<contacts::ContactRecord>>
readContactList(network::Packet& packet, uint32_t& listMask);
/// SMSG_FRIEND_LIST (Vanilla): uint8 count, friend rows without notes.
std::optional<std::vector<contacts::ContactRecord>> readFriendList(network::Packet& packet);
/// SMSG_IGNORE_LIST (Vanilla): uint8 count, guids only.
std::optional<std::vector<uint64_t>> readIgnoreList(network::Packet& packet);
contacts::FriendStatusUpdate toFriendStatusUpdate(const FriendStatusData& data);

/// What the player sees of the contact events: the chat lines, the error
/// line and FRIENDLIST_UPDATE, FRIENDLIST_SHOW and IGNORELIST_UPDATE.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeContactNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
