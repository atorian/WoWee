#pragma once

#include "core/event_bus.hpp"
#include "pet/services/ports.hpp"

#include <vector>

namespace wowee::game {

class PacketSender;
class ChatLog;
class FrameXmlEvents;
class PetQueries;

/// The pet's port, implemented over the world connection.
class PetGateway final : public pet::IPetGateway {
public:
    explicit PetGateway(PacketSender& sender) : sender_(sender) {}

    [[nodiscard]] bool online() const override;
    [[nodiscard]] bool connected() const override;
    void sendAction(uint64_t petGuid, uint32_t action, uint64_t targetGuid) override;
    void sendAutocast(uint64_t petGuid, uint32_t spellId, bool on) override;
    void sendRename(uint64_t petGuid, const std::string& name) override;
    void sendAbandon(uint64_t petGuid) override;
    void sendUnlearnTalents() override;
    void requestStableList(uint64_t masterGuid) override;
    void sendStable(uint64_t masterGuid, uint8_t slot) override;
    void sendUnstable(uint64_t masterGuid, uint32_t petNumber) override;
    void sendBuyStableSlot(uint64_t masterGuid) override;

private:
    PacketSender& sender_;
};

/// What the player and the interface are told of the pet and the stable: the
/// pet frame's and the pet bar's events, the stable window's, and the chat and
/// error lines, in the order they had. `names` names the spell a pet learns.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribePetNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, PetQueries& names);

} // namespace wowee::game
