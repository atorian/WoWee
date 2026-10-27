#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::achievement {

/// The whole list arrived (RECEIVED_ACHIEVEMENT_LIST). It comes once at login,
/// well before anything opens the panel, which builds its tree on this.
struct AchievementListReceived {};
/// The player earned an achievement (ACHIEVEMENT_EARNED).
struct AchievementEarned { uint32_t achievementId = 0; };
/// A criteria's counter moved (CRITERIA_UPDATE).
struct CriteriaProgressed { uint32_t criteriaId = 0; uint64_t progress = 0; };
/// A timed criteria reported how far into its window the player is
/// (TRACKED_ACHIEVEMENT_UPDATE). How long the window is comes from the DBC.
struct TimedCriteriaUpdated {
    uint32_t achievementId = 0;
    uint32_t criteriaId = 0;
    uint32_t elapsed = 0;
    uint32_t timeLimit = 0;
};
/// Another player's achievements arrived for the comparison tab
/// (INSPECT_ACHIEVEMENT_READY).
struct InspectAchievementsReady {};

using AchievementChange = std::variant<AchievementListReceived, AchievementEarned, CriteriaProgressed,
                                       TimedCriteriaUpdated, InspectAchievementsReady>;

// ---- Published by the service ----

/// Somebody on the realm was the first to earn an achievement.
struct ServerFirstAchievement { std::string playerName; uint32_t achievementId = 0; };

} // namespace wowee::achievement
