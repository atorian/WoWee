#pragma once

#include "foundation/event_bus.hpp"
#include "petition/domain/records.hpp"
#include "petition/services/ports.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

class PacketSender;
class ChatLog;
class FrameXmlEvents;
struct PetitionShowlistData;

/// The petition service's port, implemented over the world connection.
class PetitionGateway final : public petition::IPetitionGateway {
public:
    explicit PetitionGateway(PacketSender& sender) : sender_(sender) {}

    [[nodiscard]] bool online() const override;
    [[nodiscard]] bool connected() const override;
    void sendBuy(uint64_t npcGuid, const std::string& name, uint32_t clientIndex) override;
    void sendSign(uint64_t petitionGuid) override;
    void sendTurnIn(uint64_t petitionGuid) override;
    void sendOffer(uint64_t petitionGuid, uint64_t targetGuid) override;
    void sendRename(uint64_t petitionGuid, const std::string& name) override;
    void sendQuery(uint32_t entry, uint64_t petitionGuid) override;

private:
    PacketSender& sender_;
};

/// SMSG_PETITION_SHOWLIST as the charters take it.
[[nodiscard]] petition::CharterOffer toCharterOffer(const PetitionShowlistData& data);
/// The server's numbers for a refusal, as AzerothCore writes them.
[[nodiscard]] petition::SignRefusal signRefusal(uint32_t result);
[[nodiscard]] petition::TurnInRefusal turnInRefusal(uint32_t result);
[[nodiscard]] petition::OfferRefusal offerRefusal(uint32_t error);

/// What the player sees of the charters: the registrar panels and
/// PETITION_SHOW, the chat lines and the refusals on the error line.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribePetitionNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
