#pragma once

#include "core/event_bus.hpp"
#include "title/services/ports.hpp"

#include <vector>

namespace wowee::pipeline { class AssetManager; }

namespace wowee::game {

class PacketSender;
class ChatLog;
class FrameXmlEvents;
class SocialQueries;

/// CharTitles.dbc, read through the asset manager.
class TitleData final : public title::ITitleData {
public:
    explicit TitleData(pipeline::AssetManager* assets) : assets_(assets) {}

    [[nodiscard]] bool ready() const override;
    std::vector<title::TitleRecord> titles() override;

private:
    pipeline::AssetManager* assets_;
};

/// The titles' port: who the player is, and the request to wear a title over
/// the world connection.
class TitleGateway final : public title::ITitleGateway {
public:
    TitleGateway(PacketSender& sender, SocialQueries& names) : sender_(sender), names_(names) {}

    [[nodiscard]] bool online() const override;
    [[nodiscard]] std::string playerName() const override;
    void sendSetTitle(int32_t bit) override;

private:
    PacketSender& sender_;
    SocialQueries& names_;
};

/// What the player sees of the titles: KNOWN_TITLES_UPDATE for the character
/// sheet's dropdown, and the chat line when one is earned or taken away, in
/// that order.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeTitleNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
