#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::lfg {

enum class LfgState : uint8_t {
    None           = 0,
    RoleCheck      = 1,
    Queued         = 2,
    Proposal       = 3,
    Boot           = 4,
    InDungeon      = 5,
    FinishedDungeon= 6,
    RaidBrowser    = 7,
};

struct LfgRewardItem {
    uint32_t itemId = 0;
    uint32_t displayInfoId = 0;
    uint32_t count = 0;
};

/// What one random dungeon pays out, from SMSG_LFG_PLAYER_INFO.
struct LfgReward {
    uint32_t dungeonId = 0;
    bool done = false;          // already claimed today
    uint32_t money = 0;
    uint32_t experience = 0;
    std::vector<LfgRewardItem> items;
};

/// One row of the dungeon-ready dialog: who the proposal is offering, and
/// whether they have answered it yet.
///
/// The server sends no names or levels here - only what each player is for and
/// what they have said - so those stay empty rather than being invented.
struct LfgProposalMember {
    uint32_t role = 0;      // LFG role mask, same bits SetLFGRoles sends
    bool isSelf = false;
    bool inDungeon = false;
    bool sameGroup = false;
    bool answered = false;
    bool accepted = false;
};

/// What a finished dungeon-finder run paid out, from SMSG_LFG_PLAYER_REWARD.
///
/// Kept because the alert frame reads it back rather than being handed it:
/// LFG_COMPLETION_REWARD carries no arguments, and
/// DungeonCompletionAlertFrame_ShowAlert then asks GetLFGCompletionReward for
/// all nine values and GetLFGCompletionRewardItem for each item.
struct LfgCompletionReward {
    struct Item {
        uint32_t itemId = 0;
        uint32_t displayId = 0;
        uint32_t count = 0;
    };
    uint32_t randomDungeonId = 0;  ///< the "Random ..." entry that was queued for
    uint32_t dungeonId = 0;        ///< the dungeon actually finished
    bool done = false;
    uint32_t money = 0;            ///< copper
    uint32_t xp = 0;
    std::vector<Item> items;
    bool valid = false;
};

struct LfgRoleCheckDungeon {
    uint32_t dungeonId = 0;
    uint8_t typeId = 0;
};

/// SMSG_LFG_QUEUE_STATUS. Waits are seconds, and -1 is the server saying it
/// does not know yet: the panel tests for it, where a zero claims the group
/// is instant.
struct QueueStatus {
    uint32_t dungeonId = 0;
    /// The average across everyone queued for this dungeon, and the estimate
    /// for this player in the role they picked; the panel shows both.
    int32_t avgWaitSec = -1;
    int32_t myWaitSec = -1;
    int32_t waitTank = -1;
    int32_t waitHealer = -1;
    int32_t waitDps = -1;
    uint8_t needTank = 0;
    uint8_t needHealer = 0;
    uint8_t needDps = 0;
    uint32_t queuedSeconds = 0;
};

/// SMSG_LFG_PROPOSAL_UPDATE.
struct Proposal {
    uint32_t dungeonId = 0;
    /// 0 the proposal arriving, 1 it failing, 2 everyone accepting.
    uint32_t state = 0;
    uint32_t proposalId = 0;
    /// The player is already in the group being proposed to and has nothing
    /// to answer.
    bool silent = false;
    std::vector<LfgProposalMember> members;
};

/// SMSG_LFG_ROLE_CHECK_UPDATE.
struct RoleCheck {
    uint32_t state = 0;
    std::vector<LfgRoleCheckDungeon> dungeons;
    uint8_t members = 0;
};

/// SMSG_LFG_UPDATE_PLAYER and SMSG_LFG_UPDATE_PARTY, both read to the same
/// shape: what kind of update it is, and the queue state its body states.
struct QueueUpdate {
    uint8_t updateType = 0;
    /// Whether the packet had a body. No body means no state.
    bool extraInfo = false;
    bool queued = false;
    bool inDungeon = false;
    /// The first dungeon the update names, which the queue window shows.
    std::optional<uint32_t> dungeonId;
};

/// SMSG_LFG_BOOT_PROPOSAL_UPDATE.
struct BootVote {
    bool inProgress = false;
    bool didVote = false;
    bool myVote = false;
    uint64_t victimGuid = 0;
    uint32_t totalVotes = 0;
    uint32_t bootVotes = 0;
    uint32_t timeLeft = 0;
    uint32_t votesNeeded = 0;
    /// Absent when the packet ends before it.
    std::optional<std::string> reason;
};

/// Why the server would not queue the player (LFGJoinResult, 3.3.5a).
enum class JoinRefusal {
    RoleCheckFailed,
    GroupFull,
    NoLfgObject,
    NoSlotsPlayer,
    NoSlotsParty,
    RequirementsNotMet,
    MixedRealms,
    MembersNotPresent,
    InfoTimeout,
    InvalidSlot,
    Deserter,
    PartyDeserter,
    RandomCooldown,
    PartyRandomCooldown,
    InternalError,
    NoRole,
    Other,
};

/// Why the server would not teleport the player to or from the dungeon.
enum class TeleportRefusal { NotInGroup, NotInDungeon, SummonPending, Dead, Deserter, RequirementsNotMet, Other };

} // namespace wowee::lfg
