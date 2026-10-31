#include "game/title_gateway.hpp"

#include <cstdio>
#include <string>

#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/world_packets.hpp"
#include "game/game_data.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "title/domain/events.hpp"

namespace wowee::game {


std::vector<title::TitleRecord> TitleData::titles() {
    std::vector<title::TitleRecord> out;
    if (!data_.has("CharTitles.dbc")) return out;
    const auto dbc = data_.table("CharTitles.dbc", 5, "the titles' names");

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("CharTitles") : nullptr;
    uint32_t titleField = layout ? layout->field("Title")    : 2;
    uint32_t bitField   = layout ? layout->field("TitleBit") : 36;
    if (titleField == 0xFFFFFFFF) titleField = 2;
    if (bitField   == 0xFFFFFFFF) bitField   = static_cast<uint32_t>(dbc->getFieldCount() - 1);

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        title::TitleRecord r;
        r.id = dbc->getUInt32(i, 0);
        r.format = dbc->getString(i, titleField);
        if (r.format.empty()) continue;
        r.bit = dbc->getUInt32(i, bitField);
        out.push_back(std::move(r));
    }
    LOG_INFO("CharTitles: loaded ", out.size(), " titles from DBC");
    return out;
}

bool TitleGateway::online() const {
    return sender_.characterInWorld();
}

std::string TitleGateway::playerName() const {
    return names_.playerName(names_.characterGuid());
}

void TitleGateway::sendSetTitle(int32_t bit) {
    sender_.send(SetTitlePacket::build(bit));
    LOG_INFO("sendSetTitle: bit=", bit);
}

std::vector<core::EventBus::Subscription>
subscribeTitleNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace title;
    std::vector<core::EventBus::Subscription> s;
    // The set is read by IsTitleKnown, which the character sheet's title
    // dropdown builds itself from - but only when told to rebuild, and this is
    // what tells it. Without it a title earned mid session did not appear in
    // the list until the sheet was reopened.
    s.push_back(events.subscribe<KnownTitlesChanged>([&frameXml](const auto&) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent("KNOWN_TITLES_UPDATE", {});
    }));
    s.push_back(events.subscribe<TitleEarned>([&chat](const TitleEarned& e) {
        std::string msg;
        if (!e.title.empty()) {
            msg = e.lost ? ("Title removed: " + e.title + ".") : ("Title earned: " + e.title + "!");
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), e.lost ? "Title removed (bit %u)." : "Title earned (bit %u)!", e.bit);
            msg = buf;
        }
        chat.addSystemChatMessage(msg);
    }));
    return s;
}

} // namespace wowee::game
