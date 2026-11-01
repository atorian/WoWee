#pragma once

#include "core/event_bus.hpp"
#include "group/domain/records.hpp"
#include "group/services/ports.hpp"

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
struct AuraSlot;

/// The group service's port, implemented over the world connection.
class GroupGateway final : public group::IGroupGateway {
public:
    GroupGateway(PacketSender& sender, SocialQueries& world) : sender_(sender), world_(world) {}

    [[nodiscard]] bool canSend() const override;
    [[nodiscard]] uint64_t playerGuid() const override;
    void sendInvite(const std::string& name) override;
    void sendAccept() override;
    void sendDecline() override;
    void sendUninvite(const std::string& name) override;
    void sendLeave() override;
    void sendConvertToRaid() override;
    void sendLeader(uint64_t guid) override;
    void sendLootMethod(uint32_t method, uint64_t masterGuid, uint32_t threshold) override;
    void sendAssignment(uint8_t assignment, uint64_t guid, bool apply) override;
    void sendAssistant(uint64_t guid, bool apply) override;
    void sendSubgroup(const std::string& name, uint8_t group) override;
    void sendSwapSubgroup(const std::string& first, const std::string& second) override;
    void sendRaidMark(uint8_t icon, uint64_t guid) override;
    void sendReadyCheck() override;
    void sendReadyCheckAnswer(bool ready) override;

private:
    PacketSender& sender_;
    SocialQueries& world_;
};

/// SMSG_PARTY_MEMBER_STATS(_FULL) for whichever expansion is active. A member's
/// auras, when the packet carries them, go to `auras`; nothing when the packet
/// is cut short before the fields. `dispelTypes` answers a 1.12 aura's dispel
/// type, which that expansion does not send.
[[nodiscard]] std::optional<group::MemberStats> readPartyMemberStats(network::Packet& packet, bool isFull,
                                                                     SocialQueries& dispelTypes,
                                                                     std::optional<std::vector<AuraSlot>>& auras);

/// What the player sees of the group: the chat lines, the addon events and the
/// refusals on the error line.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeGroupNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
