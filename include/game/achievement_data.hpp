#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "achievement/services/ports.hpp"
#include "core/event_bus.hpp"

namespace wowee::pipeline { class AssetManager; }

namespace wowee::game {

class ChatLog;
class FrameXmlEvents;

/// The achievements' reference data, read from Achievement.dbc,
/// Achievement_Category.dbc and Achievement_Criteria.dbc.
class AchievementData final : public achievement::IAchievementData {
public:
    explicit AchievementData(pipeline::AssetManager* assets) : assets_(assets) {}

    [[nodiscard]] bool ready() const override;
    achievement::AchievementTable achievements() override;
    std::vector<achievement::CategoryRecord> categories() override;
    std::vector<achievement::CriterionRecord> criteria() override;

private:
    pipeline::AssetManager* assets_;
};

/// What the player and the interface are told when the achievements change:
/// the chat lines and the FrameXML events, in the order they had. `nameOf`
/// names an achievement, empty when it is not known.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeAchievementNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                            std::function<std::string(uint32_t)> nameOf);

} // namespace wowee::game
