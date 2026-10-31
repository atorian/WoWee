#include "game/achievement_data.hpp"

#include <cstdio>

#include "achievement/domain/events.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_data.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

namespace wowee::game {

namespace {
constexpr uint32_t kNoField = 0xFFFFFFFFu;
}


achievement::AchievementTable AchievementData::achievements() {
    achievement::AchievementTable table;
    if (!data_.has("Achievement.dbc")) return table;
    const auto dbc = data_.table("Achievement.dbc", 22, "the achievements' names, points and icons");

    const auto* achL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("Achievement") : nullptr;
    uint32_t titleField = achL ? achL->field("Title") : 4;
    if (titleField == kNoField) titleField = 4;
    const uint32_t descField = achL ? achL->field("Description") : kNoField;
    const uint32_t ptsField  = achL ? achL->field("Points")      : kNoField;
    // IconID references SpellIcon.dbc for the achievement's artwork. Field 42 in the
    // stock 3.3.5a Achievement.dbc when the layout doesn't name it.
    uint32_t iconField = achL ? achL->field("IconID") : kNoField;
    if (iconField == kNoField && dbc->getFieldCount() > 42) iconField = 42;

    const uint32_t fieldCount = dbc->getFieldCount();
    table.hasCategories = fieldCount > 38;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        achievement::AchievementRecord r;
        r.id = dbc->getUInt32(i, 0);
        if (r.id == 0) continue;
        r.title = dbc->getString(i, titleField);
        if (descField != kNoField && descField < fieldCount) r.description = dbc->getString(i, descField);
        if (ptsField != kNoField && ptsField < fieldCount) r.points = dbc->getUInt32(i, ptsField);
        if (iconField != kNoField && iconField < fieldCount) r.iconId = dbc->getUInt32(i, iconField);
        // Field 41 is Flags. Bit 0x1 agrees with "does this row's category
        // descend from Statistics" on all 1817 rows, and FrameXML's own
        // constants.lua names that same bit ACHIEVEMENT_FLAGS_STATISTIC.
        if (fieldCount > 41) r.flags = dbc->getUInt32(i, 41);
        if (table.hasCategories) {
            // Field 38 is the category. It is not in the layout file, but the
            // fields either side of it are - Points at 39, Description at 21,
            // IconID at 42 - which is the stock 3.3.5a order.
            r.category = dbc->getUInt32(i, 38);
            // Field 3 is Supercedes. Checked by reading it - "Level 20" points
            // at "Level 10" and "Expert Cook" at "Journeyman Cook", and all 176
            // non-zero values are ids that exist.
            r.supercedes = dbc->getUInt32(i, 3);
        }
        table.rows.push_back(std::move(r));
    }
    LOG_INFO("Achievement: read ", table.rows.size(), " rows from Achievement.dbc");
    return table;
}

std::vector<achievement::CategoryRecord> AchievementData::categories() {
    std::vector<achievement::CategoryRecord> out;
    // Achievement_Category.dbc: id, parent, then the localised names.
    if (!data_.has("Achievement_Category.dbc")) return out;
    const auto dbc = data_.table("Achievement_Category.dbc", 3, "the achievement panel's categories");
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        achievement::CategoryRecord c;
        c.id = dbc->getUInt32(i, 0);
        c.parent = dbc->getUInt32(i, 1);
        c.name = dbc->getString(i, 2);
        // Field 19 is where a category sits among its siblings. Record order
        // is not that, so the tree would otherwise read scrambled.
        if (dbc->getFieldCount() > 19) c.uiOrder = dbc->getUInt32(i, 19);
        out.push_back(std::move(c));
    }
    LOG_INFO("Achievement: ", out.size(), " categories");
    return out;
}

std::vector<achievement::CriterionRecord> AchievementData::criteria() {
    std::vector<achievement::CriterionRecord> out;
    if (!data_.has("Achievement_Criteria.dbc")) return out;
    const auto dbc = data_.table("Achievement_Criteria.dbc", 10,
                                 "the achievements' criteria, statistics and timers");

    const auto* critL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("AchievementCriteria") : nullptr;
    auto fieldOr = [&](const char* name, uint32_t fallback) {
        const uint32_t f = critL ? critL->field(name) : kNoField;
        return (f == kNoField) ? fallback : f;
    };
    const uint32_t achField  = fieldOr("AchievementID", 1);
    const uint32_t qtyField  = fieldOr("Quantity", 4);
    const uint32_t descField = fieldOr("Description", 9);
    // Field 29 holds the timer, where there is one. Fifty-nine criteria have a
    // non-zero value and every one is a round number of seconds that the
    // criteria's own description states in words - 420 against "Win Warsong
    // Gulch in under 7 minutes", 1200 against "Kill Maexxna within 20 minutes",
    // 60 against "Kill 100 Risen Zombies in 1 minute". No other field in the
    // record agrees with the text that way.
    const uint32_t limitField = fieldOr("TimeLimit", 29);
    // Field 26, identified the same way field 29 was: the hundred and sixty
    // rows with bit one set are the "Complete N quests" family that WoW draws
    // as bars, and the large-quantity rows without it - weapon skills at four
    // hundred, reputations at forty-two thousand - are the ones it draws as
    // text. Fields 9 to 25 are the sixteen locale strings and their mask, so
    // this is the first word past them.
    const uint32_t flagsField = fieldOr("Flags", 26);
    const uint32_t fieldCount = dbc->getFieldCount();

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        achievement::CriterionRecord r;
        r.achievementId = dbc->getUInt32(i, achField);
        if (r.achievementId == 0) continue;
        auto& c = r.criterion;
        c.id = dbc->getUInt32(i, 0);
        if (fieldCount > 2) c.type    = dbc->getUInt32(i, 2);
        if (fieldCount > 3) c.assetId = dbc->getUInt32(i, 3);
        if (qtyField  < fieldCount) c.quantity    = dbc->getUInt32(i, qtyField);
        if (descField < fieldCount) c.description = dbc->getString(i, descField);
        if (limitField < fieldCount) c.timeLimit = dbc->getUInt32(i, limitField);
        if (flagsField < fieldCount) c.flags = dbc->getUInt32(i, flagsField);
        out.push_back(std::move(r));
    }
    LOG_INFO("Achievement: ", out.size(), " criteria");
    return out;
}

std::vector<core::EventBus::Subscription>
subscribeAchievementNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                            std::function<std::string(uint32_t)> nameOf) {
    using namespace achievement;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    // Only the player's own: somebody else's arrives as CHAT_MSG_ACHIEVEMENT,
    // which the server sends separately and this client already handles.
    // ACHIEVEMENT_EARNED is what raises FrameXML's badge. The alert and the
    // banner are the client's, on the same event.
    s.push_back(events.subscribe<AchievementEarned>([line, addon, nameOf](const AchievementEarned& e) {
        const std::string name = nameOf(e.achievementId);
        char buf[256];
        if (!name.empty())
            std::snprintf(buf, sizeof(buf), "Achievement earned: %s", name.c_str());
        else
            std::snprintf(buf, sizeof(buf), "Achievement earned! (ID %u)", e.achievementId);
        line(buf);
        addon("ACHIEVEMENT_EARNED", {std::to_string(e.achievementId)});
    }));
    s.push_back(events.subscribe<AchievementListReceived>([addon](const auto&) {
        addon("RECEIVED_ACHIEVEMENT_LIST");
    }));
    // For achievement tracking addons.
    s.push_back(events.subscribe<CriteriaProgressed>([addon](const CriteriaProgressed& e) {
        addon("CRITERIA_UPDATE", {std::to_string(e.criteriaId), std::to_string(e.progress)});
    }));
    // The interface reads the pair and works out a start time from them.
    s.push_back(events.subscribe<TimedCriteriaUpdated>([addon](const TimedCriteriaUpdated& e) {
        addon("TRACKED_ACHIEVEMENT_UPDATE", {std::to_string(e.achievementId), std::to_string(e.criteriaId),
                                             std::to_string(e.elapsed), std::to_string(e.timeLimit)});
    }));
    // The comparison tab reads its totals on this and nothing else, so
    // without it the panel sat on whatever it had when it opened.
    s.push_back(events.subscribe<InspectAchievementsReady>([addon](const auto&) {
        addon("INSPECT_ACHIEVEMENT_READY");
    }));
    s.push_back(events.subscribe<ServerFirstAchievement>([line, nameOf](const ServerFirstAchievement& e) {
        const std::string name = nameOf(e.achievementId);
        char buf[256];
        if (!name.empty())
            std::snprintf(buf, sizeof(buf), "%s is the first on the realm to earn: %s!",
                          e.playerName.c_str(), name.c_str());
        else
            std::snprintf(buf, sizeof(buf), "%s is the first on the realm to earn achievement #%u!",
                          e.playerName.c_str(), e.achievementId);
        line(buf);
    }));
    return s;
}

} // namespace wowee::game
