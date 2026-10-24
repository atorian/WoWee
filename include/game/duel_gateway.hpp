#pragma once

#include "core/event_bus.hpp"
#include "duel/services/ports.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

class PacketSender;
class SocialQueries;
class ChatLog;
class FrameXmlEvents;

/// The duel service's ports, implemented over the world connection, the
/// units in view and the client's name cache.
class DuelGateway final : public duel::IDuelGateway, public duel::IDuelWorld {
public:
    DuelGateway(PacketSender& sender, SocialQueries& world) : sender_(sender), world_(world) {}

    // IDuelGateway
    [[nodiscard]] bool online() const override;
    void sendAccept(uint64_t arbiter) override;
    void sendCancel(uint64_t arbiter) override;
    void sendPropose(uint64_t target) override;

    // IDuelWorld
    [[nodiscard]] uint64_t playerGuid() const override;
    [[nodiscard]] std::string name(uint64_t guid) const override;

private:
    PacketSender& sender_;
    SocialQueries& world_;
};

/// What the player sees of a duel: the DUEL_* events, the chat lines, and the
/// refusals and the out-of-bounds warning on the error line.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeDuelNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
