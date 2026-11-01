#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wowee::battleground {

/// One queue slot's worth of SMSG_BATTLEFIELD_STATUS.
struct BattlefieldStatus {
    uint32_t queueSlot = 0;
    uint8_t  arenaType = 0;
    uint32_t bgTypeId = 0;
    uint8_t  minLevel = 0;
    uint8_t  maxLevel = 0;
    uint32_t instanceId = 0;
    bool     isRated = false;
    /// 0 none, 1 queued, 2 invited, 3 in progress, 4 waiting to leave.
    uint32_t statusId = 0;
    // Whichever of these the status carries; the rest stay zero.
    uint32_t avgWaitMs = 0;        ///< status 1
    uint32_t timeInQueueMs = 0;    ///< status 1
    uint32_t mapId = 0;            ///< status 2 and 3
    uint32_t inviteTimeoutMs = 0;  ///< status 2 - how long the invitation lasts
};

struct BgQueueSlot {
    uint32_t queueSlot = 0;
    uint32_t bgTypeId = 0;
    uint8_t arenaType = 0;
    uint32_t statusId = 0;
    uint32_t inviteTimeout = 80;
    uint32_t avgWaitTimeSec = 0;
    uint32_t timeInQueueSec = 0;
    // The level range comes with the status, so a queued battleground can say
    // what it is without the available-battleground list having arrived - that
    // list only turns up at a battlemaster, and the interface asks for the range
    // every time it draws the queue.
    uint32_t minLevel = 0;
    uint32_t maxLevel = 0;
    // Both are in the status packet too. The instance number is what makes the
    // queue read "Warsong Gulch 2" rather than "Warsong Gulch", and rated is
    // what tells a rated arena from a casual one.
    uint32_t instanceId = 0;
    bool     isRated = false;
    std::chrono::steady_clock::time_point inviteReceivedTime{};
    // When timeInQueueSec was last told. The server says it about once a
    // minute; the time waited counts on from here in between.
    std::chrono::steady_clock::time_point queueTimeReceivedAt{};
    std::string bgName;
};

/// SMSG_BATTLEFIELD_LIST: the instances of one battleground running now.
struct AvailableBgInfo {
    uint32_t bgTypeId         = 0;
    bool     isRegistered     = false;
    bool     isHoliday        = false;
    uint32_t minLevel         = 0;
    uint32_t maxLevel         = 0;
    std::vector<uint32_t> instanceIds;
};

struct BgPlayerScore {
    uint64_t    guid            = 0;
    std::string name;
    uint8_t     team            = 0;
    uint32_t    killingBlows    = 0;
    uint32_t    deaths          = 0;
    uint32_t    honorableKills  = 0;
    uint32_t    bonusHonor      = 0;
    uint32_t    damageDone      = 0;
    /// Both are on the wire and both were being skipped, which is why the
    /// scoreboard showed two columns of zeros.
    uint32_t    healingDone     = 0;
    /// Whether the team above came from the packet. Arenas carry a team byte
    /// and battlegrounds do not, so a battleground row has no faction to give
    /// and saying so is better than answering zero as though it meant Horde.
    bool        hasTeam         = false;
    /// The objective values, in the order the battleground writes them. The
    /// names are not on the wire - this used to read one before each value and
    /// took every value after the first from the wrong offset.
    std::vector<std::pair<std::string, uint32_t>> bgStats;
};

struct ArenaTeamScore {
    std::string teamName;
    uint32_t    ratingChange = 0;
    uint32_t    newRating    = 0;
};

/// MSG_PVP_LOG_DATA: the scoreboard.
struct BgScoreboardData {
    std::vector<BgPlayerScore> players;
    bool hasWinner = false;
    uint8_t winner = 0;
    bool isArena   = false;
    ArenaTeamScore arenaTeams[2];
};

struct BgPlayerPosition {
    uint64_t guid  = 0;
    float    wowX  = 0.0f;
    float    wowY  = 0.0f;
    int      group = 0;
};

/// Why the server refused to queue the player or the group
/// (SMSG_GROUP_JOINED_BATTLEGROUND's negative results, and zero).
enum class JoinRefusal {
    NotEligible,
    Deserter,
    ArenaPartySize,
    TooManyQueues,
    RatedWhileQueued,
    QueuedWhileRated,
    TeamLeftArenaQueue,
    InBattleground,
    LevelRange,
    /// A party member could not join; the refusal names them.
    MemberUnavailable,
    GroupJoinFailed,
    InDungeonSystem,
    InRandomQueue,
    RandomWhileQueued,
    Other,
};

/// The battleground's name by its type id, or empty for one not known.
[[nodiscard]] std::string battlegroundName(uint32_t bgTypeId);

} // namespace wowee::battleground
