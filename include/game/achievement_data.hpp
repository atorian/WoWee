#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "achievement/services/ports.hpp"
#include "core/event_bus.hpp"

namespace wowee::game {

class GameData;
class ChatLog;
class FrameXmlEvents;

/// The achievements' reference data, read from Achievement.dbc,
/// Achievement_Category.dbc and Achievement_Criteria.dbc through the game's
/// tables. An expansion without achievements (before WotLK) has none of them
/// and answers empty; one that has them and cannot read them stops the game.
class AchievementData final : public achievement::IAchievementData {
public:
    explicit AchievementData(const GameData& data) : data_(data) {}

    /// Always: the tables are there whenever they are asked for, or the game
    /// stops.
    [[nodiscard]] bool ready() const override { return true; }
    achievement::AchievementTable achievements() override;
    std::vector<achievement::CategoryRecord> categories() override;
    std::vector<achievement::CriterionRecord> criteria() override;

private:
    const GameData& data_;
};

/// What the player and the interface are told when the achievements change:
/// the chat lines and the FrameXML events, in the order they had. `nameOf`
/// names an achievement, empty when it is not known.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeAchievementNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                            std::function<std::string(uint32_t)> nameOf);

} // namespace wowee::game
