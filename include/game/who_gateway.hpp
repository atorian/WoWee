#pragma once

#include "core/event_bus.hpp"
#include "who/services/ports.hpp"

#include <vector>

namespace wowee::game {

class PacketSender;
class ChatLog;
class FrameXmlEvents;
class SocialQueries;

/// The /who service's port, implemented over the world connection.
class WhoGateway final : public who::IWhoGateway {
public:
    explicit WhoGateway(PacketSender& sender) : sender_(sender) {}

    [[nodiscard]] bool online() const override;
    void sendQuery(const std::string& playerName) override;

private:
    PacketSender& sender_;
};

/// What the player sees of a /who answer: WHO_LIST_UPDATE for the panel, the
/// rows in the chat when no panel shows them, and "No players found." on the
/// error line. `names` gives each row's zone its name.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeWhoNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, SocialQueries& names);

} // namespace wowee::game
