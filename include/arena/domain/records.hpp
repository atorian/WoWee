#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::arena {

struct ArenaTeamStats {
    uint32_t teamId       = 0;
    uint32_t rating       = 0;
    uint32_t weekGames    = 0;
    uint32_t weekWins     = 0;
    uint32_t seasonGames  = 0;
    uint32_t seasonWins   = 0;
    uint32_t rank         = 0;
    std::string teamName;
    uint32_t teamType     = 0;
};

struct ArenaTeamMember {
    uint64_t    guid            = 0;
    std::string name;
    bool        online          = false;
    /// The three the roster carries between the name and the record. They were
    /// on the wire all along and read as part of the games played.
    bool        isCaptain       = false;
    uint8_t     level           = 0;
    uint8_t     classId         = 0;
    uint32_t    weekGames       = 0;
    uint32_t    weekWins        = 0;
    uint32_t    seasonGames     = 0;
    uint32_t    seasonWins      = 0;
    uint32_t    personalRating  = 0;
};

struct ArenaTeamRoster {
    uint32_t teamId = 0;
    std::vector<ArenaTeamMember> members;
};

/// Which arena team command the server is answering.
enum class ArenaCommand { Create, Invite, Quit, Captain, Other };

/// Why the server refused an arena team command.
enum class ArenaCommandError {
    Internal,
    AlreadyInTeamOfSize,
    TargetAlreadyInTeamOfSize,
    AlreadyInvited,
    TargetAlreadyInvited,
    NameInvalid,
    NameTaken,
    /// The server sends the same refusal for the captain leaving and for
    /// acting without the rank to do so.
    CaptainCannotLeaveOrNoPermission,
    NotInTeamOfSize,
    TargetNotInTeam,
    PlayerNotFound,
    TargetOpposingFaction,
    TargetIgnoringYou,
    TargetLevelTooLow,
    TargetLevelTooHigh,
    TeamFull,
    TeamNotFound,
    LockedForSeason,
    Other,
};

/// What happened to one of the player's arena teams (SMSG_ARENA_TEAM_EVENT).
enum class ArenaTeamChange { Joined, Left, Removed, CaptainChanged, Disbanded, Created, Other };

/// Why the server would not start an arena match (SMSG_ARENA_ERROR).
enum class ArenaMatchError { OtherTeamTooSmall, TeamFull, NotEnoughMembers, TooManyMembers, Other };

} // namespace wowee::arena
