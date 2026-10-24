#include "game/contacts_gateway.hpp"

#include "contacts/services/contacts_service.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_utils.hpp"
#include "game/opcode_table.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee::game {

namespace ContactFlag = contacts::ContactFlag;
using contacts::ContactRecord;

bool ContactsGateway::canSend() const {
    return sender_.characterInWorld();
}

void ContactsGateway::sendAddFriend(const std::string& name, const std::string& note) {
    sender_.send(AddFriendPacket::build(name, note));
    LOG_INFO("Sent friend request to: ", name);
}

void ContactsGateway::sendRemoveFriend(uint64_t guid) {
    sender_.send(DelFriendPacket::build(guid));
    LOG_INFO("Sent remove friend request for guid 0x", std::hex, guid, std::dec);
}

void ContactsGateway::sendFriendNote(uint64_t guid, const std::string& note) {
    sender_.send(SetContactNotesPacket::build(guid, note));
    LOG_INFO("Set friend note for guid 0x", std::hex, guid, std::dec);
}

void ContactsGateway::sendAddIgnore(const std::string& name) {
    sender_.send(AddIgnorePacket::build(name));
    LOG_INFO("Sent ignore request for: ", name);
}

void ContactsGateway::sendRemoveIgnore(uint64_t guid) {
    sender_.send(DelIgnorePacket::build(guid));
    LOG_INFO("Sent remove ignore request for guid 0x", std::hex, guid, std::dec);
}

void ContactsGateway::sendContactListRequest() {
    // Once a second. FriendsFrame_Update calls ShowFriends every time it
    // redraws, and it redraws on every friend status line - so an unthrottled
    // send answers each one with a request for the whole list again.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastListRequest_ < std::chrono::seconds(1)) return;
    lastListRequest_ = now;
    // Two names for one request. Vanilla calls it CMSG_FRIEND_LIST and sends
    // it empty; TBC renamed it CMSG_CONTACT_LIST and gave it a flags word.
    // Checked against the opcode maps: classic names only the first, TBC and
    // WotLK only the second.
    if (isClassicLikeExpansion()) {
        const uint32_t wire = wireOpcode(Opcode::CMSG_FRIEND_LIST);
        if (wire == 0xFFFF) return;
        network::Packet pkt(static_cast<uint16_t>(wire));
        sender_.send(pkt);
        return;
    }
    const uint32_t wire = wireOpcode(Opcode::CMSG_CONTACT_LIST);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    // Which lists to send back. AzerothCore reads one uint32 and hands it
    // straight to SendSocialList; SOCIAL_FLAG_ALL is friend, ignored and muted
    // together, which is what the panel shows across its three tabs.
    constexpr uint32_t kSocialFlagAll = 0x07;
    pkt.writeUInt32(kSocialFlagAll);
    sender_.send(pkt);
}

std::string ContactsGateway::cachedName(uint64_t guid) const {
    return names_.playerName(guid);
}

void ContactsGateway::requestName(uint64_t guid) {
    names_.requestPlayerName(guid);
}

std::optional<std::vector<ContactRecord>>
readContactList(network::Packet& packet, uint32_t& listMask) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 8) { packet.skipAll(); return std::nullopt; }
    listMask = packet.readUInt32();
    const uint32_t count = packet.readUInt32();
    std::vector<ContactRecord> records;
    for (uint32_t i = 0; i < count && rem() >= 8; ++i) {
        ContactRecord r;
        r.guid = packet.readUInt64();
        if (rem() < 4) break;
        r.flags = packet.readUInt32();
        r.note = packet.readString();
        if (r.flags & ContactFlag::Friend) {
            if (rem() < 1) break;
            r.status = packet.readUInt8();
            if (r.status != 0 && rem() >= 12) {
                r.areaId = packet.readUInt32();
                r.level = packet.readUInt32();
                r.classId = packet.readUInt32();
            }
        }
        records.push_back(std::move(r));
    }
    return records;
}

std::optional<std::vector<ContactRecord>> readFriendList(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 1) return std::nullopt;
    const uint8_t count = packet.readUInt8();
    std::vector<ContactRecord> records;
    for (uint8_t i = 0; i < count && rem() >= 9; ++i) {
        ContactRecord r;
        r.guid = packet.readUInt64();
        r.flags = ContactFlag::Friend;
        r.status = packet.readUInt8();
        if (r.status != 0 && rem() >= 12) {
            r.areaId = packet.readUInt32();
            r.level = packet.readUInt32();
            r.classId = packet.readUInt32();
        }
        records.push_back(std::move(r));
    }
    return records;
}

std::optional<std::vector<uint64_t>> readIgnoreList(network::Packet& packet) {
    // Format: uint8 count + count x uint64 guid (no name strings in packet).
    // Names are resolved via SMSG_NAME_QUERY_RESPONSE after the list arrives.
    if (!packet.hasRemaining(1)) return std::nullopt;
    const uint8_t count = packet.readUInt8();
    std::vector<uint64_t> guids;
    for (uint8_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(8)) break;
        guids.push_back(packet.readUInt64());
    }
    LOG_DEBUG("SMSG_IGNORE_LIST: loaded ", static_cast<int>(count), " ignored players");
    return guids;
}

contacts::FriendStatusUpdate toFriendStatusUpdate(const FriendStatusData& data) {
    contacts::FriendStatusUpdate u;
    u.result = data.status;
    u.guid = data.guid;
    u.note = data.note;
    u.chatFlag = data.chatFlag;
    u.areaId = data.areaId;
    u.level = data.level;
    u.classId = data.classId;
    return u;
}

std::vector<core::EventBus::Subscription>
subscribeContactNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace contacts;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](std::string text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name);
    };

    s.push_back(events.subscribe<FriendListChanged>([addon](const auto&) { addon("FRIENDLIST_UPDATE"); }));
    s.push_back(events.subscribe<FriendListShown>([addon](const auto&) { addon("FRIENDLIST_SHOW"); }));
    s.push_back(events.subscribe<IgnoreListChanged>([addon](const auto&) { addon("IGNORELIST_UPDATE"); }));

    s.push_back(events.subscribe<FriendListUnavailable>([line](const auto&) { line("Your friends list could not be reached."); }));
    s.push_back(events.subscribe<FriendListFull>([line](const auto&) { line("Your friends list is full."); }));
    s.push_back(events.subscribe<FriendCameOnline>([line](const auto& e) { line(e.name + " is now online."); }));
    s.push_back(events.subscribe<FriendWentOffline>([line](const auto& e) { line(e.name + " is now offline."); }));
    s.push_back(events.subscribe<FriendNotFound>([line](const auto&) { line("Player not found."); }));
    s.push_back(events.subscribe<FriendRemoved>([line](const auto& e) { line(e.name + " has been removed from your friends list."); }));
    s.push_back(events.subscribe<FriendAdded>([line](const auto& e) { line(e.name + " has been added to your friends list."); }));
    s.push_back(events.subscribe<AlreadyFriend>([line](const auto& e) { line(e.name + " is already in your friends list."); }));
    s.push_back(events.subscribe<CannotBefriendSelf>([line](const auto&) { line("You cannot add yourself as a friend."); }));
    s.push_back(events.subscribe<FriendIsHostile>([line](const auto& e) { line(e.name + " belongs to the other faction."); }));

    s.push_back(events.subscribe<FriendRequestSent>([line](const auto& e) { line("Sending friend request to " + e.name + "..."); }));
    s.push_back(events.subscribe<FriendRemovalRequested>([line](const auto& e) { line("Removing " + e.name + " from friends list..."); }));
    s.push_back(events.subscribe<FriendNoteSent>([line](const auto& e) { line("Updated note for " + e.name); }));
    s.push_back(events.subscribe<IgnoreRequested>([line](const auto& e) { line("Adding " + e.name + " to ignore list..."); }));
    s.push_back(events.subscribe<IgnoreRemovalRequested>([line](const auto& e) { line("Removing " + e.name + " from ignore list..."); }));
    s.push_back(events.subscribe<ContactNameRequired>([&events, &frameXml](const auto&) {
        raiseUiError(events, frameXml, "You must specify a player name.");
    }));
    s.push_back(events.subscribe<NotAFriend>([line](const auto& e) { line(e.name + " is not in your friends list."); }));
    s.push_back(events.subscribe<NotIgnored>([line](const auto& e) { line(e.name + " is not in your ignore list."); }));
    return s;
}

} // namespace wowee::game
