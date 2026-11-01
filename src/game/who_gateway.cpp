#include "game/who_gateway.hpp"

#include "core/logger.hpp"
#include "game/character.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "who/domain/who_list.hpp"

namespace wowee::game {

bool WhoGateway::online() const {
    return sender_.characterInWorld();
}

void WhoGateway::sendQuery(const std::string& playerName) {
    sender_.send(WhoPacket::build(0, 0, playerName));
    LOG_INFO("Sent WHO query", playerName.empty() ? "" : " for: " + playerName);
}

std::vector<core::EventBus::Subscription>
subscribeWhoNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, SocialQueries& names) {
    using namespace who;
    std::vector<core::EventBus::Subscription> s;
    s.push_back(events.subscribe<NoPlayersFound>([&events, &frameXml](const auto&) {
        raiseUiError(events, frameXml, "No players found.");
    }));
    // To the chat when nobody is showing a panel, which is what a stock client
    // does and what SetWhoToUI is for. The formats are WoW's own, from
    // globalstrings - and they are the client's to print rather than the
    // interface's, which is why no FrameXML file formats them. The count line
    // carries a |4 escape, which resolves on the way to the screen.
    s.push_back(events.subscribe<WhoPrinted>([&chat, &names](const WhoPrinted& e) {
        for (const auto& w : e.entries) {
            std::string line = "|Hplayer:" + w.name + "|h[" + w.name + "]|h: Level " +
                               std::to_string(w.level) + " " +
                               getRaceName(static_cast<Race>(w.raceId)) + std::string(" ") +
                               getClassName(static_cast<Class>(w.classId));
            if (!w.guildName.empty()) line += " <" + w.guildName + ">";
            const std::string zone = names.areaName(w.zoneId);
            if (!zone.empty()) line += " - " + zone;
            chat.addSystemChatMessage(line);
        }
        chat.addSystemChatMessage(std::to_string(e.onlineCount) + " |4player:players; total");
    }));
    // The who panel asked for this and redraws its rows on the answer coming
    // back.
    s.push_back(events.subscribe<WhoListUpdated>([&frameXml](const auto&) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent("WHO_LIST_UPDATE");
    }));
    return s;
}

} // namespace wowee::game
