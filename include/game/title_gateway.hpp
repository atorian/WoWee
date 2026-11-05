#pragma once

#include "foundation/event_bus.hpp"
#include "title/services/ports.hpp"

#include <vector>

namespace wowee::game {

class GameData;
class PacketSender;
class ChatLog;
class FrameXmlEvents;
class SocialQueries;

/// CharTitles.dbc, read through the game's tables. Classic has no titles and
/// answers none; an expansion that has them and cannot read them stops the
/// game.
class TitleData final : public title::ITitleData {
public:
    explicit TitleData(const GameData& data) : data_(data) {}

    /// Always: the table is there whenever it is asked for, or the game stops.
    [[nodiscard]] bool ready() const override { return true; }
    std::vector<title::TitleRecord> titles() override;

private:
    const GameData& data_;
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
