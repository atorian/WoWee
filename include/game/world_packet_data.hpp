#pragma once

// What the world protocol carries, as data: the structs a parsed packet fills
// and the game keeps as its state, their enums, and helpers that work on them
// alone. Building and parsing the packets is game/world_packets.hpp.
//
// The handlers, the interface and the client hold these and include only this
// header, so a change to how a packet is read or written rebuilds the protocol
// and not everything that shows what it carried.

#include "game/item_text.hpp"
#include "game/character.hpp"
#include "game/entity.hpp"
#include "game/movement_flags.hpp"
#include "game/spell_defines.hpp"
#include "game/group_defines.hpp"
#include "game/flat_field_map.hpp"
#include <array>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <map>
#include <unordered_map>
#include <chrono>

namespace wowee {
namespace network { class Packet; }
}

namespace wowee {
namespace game {

// Normalize WoW in-text tokens (e.g. "$B", "|n") into plain text suitable for UI.
std::string normalizeWowTextTokens(std::string text, const std::string& playerName = "");

// ============================================================
// Session - auth, characters, world entry
// ============================================================

/**
 * SMSG_AUTH_CHALLENGE data (from server)
 *
 * Sent by world server immediately after TCP connect
 * Contains server seed/salt for authentication hash
 */
struct AuthChallengeData {
    uint32_t unknown1;      // Always seems to be 0x00000001
    uint32_t serverSeed;    // Random seed from server
    // Note: 3.3.5a has additional data after this

    [[nodiscard]] bool isValid() const { return true; }
};

/**
 * SMSG_AUTH_RESPONSE result codes
 */
enum class AuthResult : uint8_t {
    // TrinityCore/AzerothCore auth response codes (3.3.5a)
    OK = 0x0C,                      // Success, proceed to character screen
    FAILED = 0x0D,                  // Generic failure
    REJECT = 0x0E,                  // Reject
    BAD_SERVER_PROOF = 0x0F,        // Bad server proof
    UNAVAILABLE = 0x10,             // Unavailable
    SYSTEM_ERROR = 0x11,            // System error
    BILLING_ERROR = 0x12,           // Billing error
    BILLING_EXPIRED = 0x13,         // Billing expired
    VERSION_MISMATCH = 0x14,        // Version mismatch
    UNKNOWN_ACCOUNT = 0x15,         // Unknown account
    INCORRECT_PASSWORD = 0x16,      // Incorrect password
    SESSION_EXPIRED = 0x17,         // Session expired
    SERVER_SHUTTING_DOWN = 0x18,    // Server shutting down
    ALREADY_LOGGING_IN = 0x19,      // Already logging in
    LOGIN_SERVER_NOT_FOUND = 0x1A,  // Login server not found
    WAIT_QUEUE = 0x1B,              // Wait queue
    BANNED = 0x1C,                  // Banned
    ALREADY_ONLINE = 0x1D,          // Already online
    NO_TIME = 0x1E,                 // No game time
    DB_BUSY = 0x1F,                 // DB busy
    SUSPENDED = 0x20,               // Suspended
    PARENTAL_CONTROL = 0x21,        // Parental control
    LOCKED_ENFORCED = 0x22,         // Account locked
    // Realm-list results, also sent in place of an auth result
    REALM_LIST_IN_PROGRESS = 0x23,
    REALM_LIST_SUCCESS = 0x24,
    REALM_LIST_FAILED = 0x25,
    REALM_LIST_INVALID = 0x26,
    REALM_LIST_REALM_NOT_FOUND = 0x27  // this world server does not host the realm id sent
};

/**
 * SMSG_AUTH_RESPONSE data (from server)
 */
struct AuthResponseData {
    AuthResult result;

    [[nodiscard]] bool isSuccess() const { return result == AuthResult::OK; }
};

/**
 * Get human-readable string for auth result
 */
const char* getAuthResultString(AuthResult result);

/**
 * SMSG_CHAR_ENUM response data (from server)
 *
 * Contains list of all characters on the account
 */
struct CharEnumResponse {
    std::vector<Character> characters;

    [[nodiscard]] bool isEmpty() const { return characters.empty(); }
    [[nodiscard]] size_t count() const { return characters.size(); }
};

// WoW 3.3.5a ResponseCodes for character creation (from ResponseCodes enum)
// Windows wingdi.h defines ERROR as 0; undefine it for this enum scope.
#ifdef _WIN32
#pragma push_macro("ERROR")
#undef ERROR
#endif
enum class CharCreateResult : uint8_t {
    // Success codes
    SUCCESS              = 0x2F,  // CHAR_CREATE_SUCCESS

    // CHAR_CREATE error codes
    IN_PROGRESS          = 0x2E,  // CHAR_CREATE_IN_PROGRESS
    CHAR_ERROR           = 0x30,  // CHAR_CREATE_ERROR
    FAILED               = 0x31,  // CHAR_CREATE_FAILED
    NAME_IN_USE          = 0x32,  // CHAR_CREATE_NAME_IN_USE
    DISABLED             = 0x33,  // CHAR_CREATE_DISABLED
    PVP_TEAMS_VIOLATION  = 0x34,  // CHAR_CREATE_PVP_TEAMS_VIOLATION
    SERVER_LIMIT         = 0x35,  // CHAR_CREATE_SERVER_LIMIT
    ACCOUNT_LIMIT        = 0x36,  // CHAR_CREATE_ACCOUNT_LIMIT
    SERVER_QUEUE         = 0x37,  // CHAR_CREATE_SERVER_QUEUE
    ONLY_EXISTING        = 0x38,  // CHAR_CREATE_ONLY_EXISTING
    EXPANSION            = 0x39,  // CHAR_CREATE_EXPANSION
    EXPANSION_CLASS      = 0x3A,  // CHAR_CREATE_EXPANSION_CLASS
    LEVEL_REQUIREMENT    = 0x3B,  // CHAR_CREATE_LEVEL_REQUIREMENT
    UNIQUE_CLASS_LIMIT   = 0x3C,  // CHAR_CREATE_UNIQUE_CLASS_LIMIT
    CHARACTER_IN_GUILD   = 0x3D,  // CHAR_CREATE_CHARACTER_IN_GUILD
    RESTRICTED_RACECLASS = 0x3E,  // CHAR_CREATE_RESTRICTED_RACECLASS
    CHARACTER_CHOOSE_RACE= 0x3F,  // CHAR_CREATE_CHARACTER_CHOOSE_RACE
    CHARACTER_ARENA_LEADER=0x40,  // CHAR_CREATE_CHARACTER_ARENA_LEADER
    CHARACTER_DELETE_MAIL= 0x41,  // CHAR_CREATE_CHARACTER_DELETE_MAIL
    CHARACTER_SWAP_FACTION=0x42,  // CHAR_CREATE_CHARACTER_SWAP_FACTION
    CHARACTER_RACE_ONLY  = 0x43,  // CHAR_CREATE_CHARACTER_RACE_ONLY
    CHARACTER_GOLD_LIMIT = 0x44,  // CHAR_CREATE_CHARACTER_GOLD_LIMIT
    FORCE_LOGIN          = 0x45,  // CHAR_CREATE_FORCE_LOGIN

    // CHAR_NAME error codes (name validation failures)
    NAME_SUCCESS         = 0x57,  // CHAR_NAME_SUCCESS
    NAME_FAILURE         = 0x58,  // CHAR_NAME_FAILURE
    NAME_NO_NAME         = 0x59,  // CHAR_NAME_NO_NAME
    NAME_TOO_SHORT       = 0x5A,  // CHAR_NAME_TOO_SHORT
    NAME_TOO_LONG        = 0x5B,  // CHAR_NAME_TOO_LONG
    NAME_INVALID_CHARACTER = 0x5C,  // CHAR_NAME_INVALID_CHARACTER
    NAME_MIXED_LANGUAGES = 0x5D,  // CHAR_NAME_MIXED_LANGUAGES
    NAME_PROFANE         = 0x5E,  // CHAR_NAME_PROFANE
    NAME_RESERVED        = 0x5F,  // CHAR_NAME_RESERVED
    NAME_INVALID_APOSTROPHE = 0x60,  // CHAR_NAME_INVALID_APOSTROPHE
    NAME_MULTIPLE_APOSTROPHES = 0x61,  // CHAR_NAME_MULTIPLE_APOSTROPHES
    NAME_THREE_CONSECUTIVE = 0x62,  // CHAR_NAME_THREE_CONSECUTIVE (98 decimal)
    NAME_INVALID_SPACE   = 0x63,  // CHAR_NAME_INVALID_SPACE
    NAME_CONSECUTIVE_SPACES = 0x64,  // CHAR_NAME_CONSECUTIVE_SPACES
    NAME_RUSSIAN_CONSECUTIVE_SILENT = 0x65,  // CHAR_NAME_RUSSIAN_CONSECUTIVE_SILENT_CHARACTERS
    NAME_RUSSIAN_SILENT_AT_BEGIN_OR_END = 0x66,  // CHAR_NAME_RUSSIAN_SILENT_CHARACTER_AT_BEGINNING_OR_END
    NAME_DECLENSION_DOESNT_MATCH = 0x67,  // CHAR_NAME_DECLENSION_DOESNT_MATCH_BASE_NAME
};
#ifdef _WIN32
#pragma pop_macro("ERROR")
#endif

struct CharCreateData {
    std::string name;
    Race race;
    Class characterClass;
    Gender gender;
    uint8_t skin = 0;
    uint8_t face = 0;
    uint8_t hairStyle = 0;
    uint8_t hairColor = 0;
    uint8_t facialHair = 0;
    bool useFemaleModel = false;  // For nonbinary: choose body type
};

struct CharCreateResponseData {
    CharCreateResult result;
};

/**
 * SMSG_LOGIN_VERIFY_WORLD data (from server)
 *
 * Confirms successful world entry with initial position and map info
 */
struct LoginVerifyWorldData {
    uint32_t mapId;         // Map ID where character spawned
    float x, y, z;          // Initial position coordinates
    float orientation;      // Initial orientation (facing direction)

    [[nodiscard]] bool isValid() const { return mapId != 0xFFFFFFFF; }
};

/**
 * SMSG_ACCOUNT_DATA_TIMES data (from server, WotLK)
 *
 * When the server last saw each account data slot change - macros, key
 * bindings, settings. Only the slots in `mask` were sent; the others read 0.
 */
struct AccountDataTimesData {
    uint32_t serverTime = 0;            // Current server time (Unix timestamp)
    uint8_t unknown = 0;                // Always 1 from TrinityCore and AzerothCore
    uint32_t mask = 0;                  // Which slots the packet carries
    uint32_t accountDataTimes[8] = {};  // Per slot, 0 where not in the mask
};

/**
 * SMSG_UPDATE_ACCOUNT_DATA data (from server, WotLK): one slot's text.
 */
struct UpdateAccountDataData {
    uint64_t guid = 0;      // The character it belongs to, 0 for the account's
    uint32_t type = 0;      // AccountDataType
    uint32_t time = 0;      // When it last changed (Unix timestamp)
    std::string text;       // Decompressed
};

/**
 * SMSG_MOTD data (from server)
 *
 * Message of the Day from server
 */
struct MotdData {
    std::vector<std::string> lines;     // MOTD text lines

    [[nodiscard]] bool isEmpty() const { return lines.empty(); }
    [[nodiscard]] size_t lineCount() const { return lines.size(); }
};

/**
 * SMSG_PONG data (from server)
 *
 * Response to CMSG_PING, echoes back the sequence number
 */
struct PongData {
    uint32_t sequence;      // Sequence number from CMSG_PING

    [[nodiscard]] bool isValid() const { return true; }
};

// ============================================================
// Movement and Objects
// ============================================================

/**
 * Movement info structure
 *
 * Contains all movement-related data sent in movement packets
 */
struct MovementInfo {
    uint32_t flags = 0;             // Movement flags
    uint16_t flags2 = 0;            // Extra movement flags
    uint32_t time = 0;              // Movement timestamp (milliseconds)
    float x = 0.0f;                 // Position X
    float y = 0.0f;                 // Position Y
    float z = 0.0f;                 // Position Z
    float orientation = 0.0f;       // Facing direction (radians)

    // Optional fields (based on flags)
    float pitch = 0.0f;             // Pitch angle (swimming/flying)
    uint32_t fallTime = 0;          // Time falling (milliseconds)
    float jumpVelocity = 0.0f;      // Jump vertical velocity
    float jumpSinAngle = 0.0f;      // Jump horizontal sin
    float jumpCosAngle = 0.0f;      // Jump horizontal cos
    float jumpXYSpeed = 0.0f;       // Jump horizontal speed
    float splineElevation = 0.0f;  // Vertical spline/step offset when carried by flags

    // Transport fields (when ONTRANSPORT flag is set)
    uint64_t transportGuid = 0;     // GUID of transport (boat/zeppelin/etc)
    float transportX = 0.0f;        // Local position on transport
    float transportY = 0.0f;
    float transportZ = 0.0f;
    float transportO = 0.0f;        // Local orientation on transport
    uint32_t transportTime = 0;     // Transport movement timestamp
    int8_t transportSeat = -1;      // Transport seat (-1 when unknown/not seated)
    uint32_t transportTime2 = 0;    // Secondary transport time (when interpolated movement flag is set)

    [[nodiscard]] bool hasFlag(MovementFlags flag) const {
        return (flags & static_cast<uint32_t>(flag)) != 0;
    }

    // What the movement flags mean. GameHandler and MovementHandler both used
    // to spell these out against their own copy of the flag word, which is one
    // definition of flying in two places.
    [[nodiscard]] bool isPlayerRooted() const { return hasFlag(MovementFlags::ROOT); }
    [[nodiscard]] bool isGravityDisabled() const { return hasFlag(MovementFlags::LEVITATING); }
    [[nodiscard]] bool isFeatherFalling() const { return hasFlag(MovementFlags::FEATHER_FALL); }
    [[nodiscard]] bool isWaterWalking() const { return hasFlag(MovementFlags::WATER_WALK); }
    [[nodiscard]] bool isHovering() const { return hasFlag(MovementFlags::HOVER); }
    [[nodiscard]] bool isSwimming() const { return hasFlag(MovementFlags::SWIMMING); }

    /// Both flags, not either. CAN_FLY on its own is permission to fly, which
    /// a player has while mounted on a flying mount and standing on the
    /// ground, and FLYING on its own arrives briefly mid-transition. Treating
    /// either as flying puts the character into the airborne animation while
    /// it is walking around.
    [[nodiscard]] bool isPlayerFlying() const {
        const uint32_t flyMask = static_cast<uint32_t>(MovementFlags::CAN_FLY) |
                                 static_cast<uint32_t>(MovementFlags::FLYING);
        return (flags & flyMask) == flyMask;
    }
};

// Forward declare Entity types
class Entity;
class EntityManager;
enum class ObjectType : uint8_t;
enum class UpdateType : uint8_t;

/**
 * Update block for a single object in SMSG_UPDATE_OBJECT
 */
struct UpdateBlock {
    UpdateType updateType;
    uint64_t guid = 0;
    ObjectType objectType;

    // Movement data (for MOVEMENT updates)
    bool hasMovement = false;
    float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    float walkSpeed = 0.0f;
    float runSpeed = 0.0f;
    float runBackSpeed = 0.0f;
    float swimSpeed = 0.0f;
    float swimBackSpeed = 0.0f;
    float flightSpeed = 0.0f;
    float flightBackSpeed = 0.0f;
    float turnRate = 0.0f;
    float pitchRate = 0.0f;

    // Update flags from movement block (for detecting transports, etc.)
    uint16_t updateFlags = 0;

    // Raw movement flags from LIVING block (SWIMMING=0x200000, WALKING=0x100, CAN_FLY=0x800000, FLYING=0x1000000)
    // Used to initialise swim/walk/fly state on entity spawn (cold-join).
    uint32_t moveFlags = 0;

    // Transport data from LIVING movement block (MOVEMENTFLAG_ONTRANSPORT)
    bool onTransport = false;
    uint64_t transportGuid = 0;
    float transportX = 0.0f, transportY = 0.0f, transportZ = 0.0f, transportO = 0.0f;

    // Field data (for VALUES and CREATE updates) - sorted flat vector instead of
    // std::map to drop the per-field heap allocation on every UPDATE_OBJECT block.
    FlatFieldMap fields;
};

/**
 * SMSG_UPDATE_OBJECT data
 *
 * Contains all update blocks in the packet
 */
struct UpdateObjectData {
    uint32_t blockCount = 0;
    std::vector<UpdateBlock> blocks;

    // Out-of-range GUIDs (for OUT_OF_RANGE_OBJECTS)
    std::vector<uint64_t> outOfRangeGuids;
};

/**
 * SMSG_DESTROY_OBJECT data
 */
struct DestroyObjectData {
    uint64_t guid = 0;
    bool isDeath = false;  // true if unit died, false if despawned
};

// ============================================================
// Chat, Emotes, Channels
// ============================================================

/**
 * Chat message types - wire values shared across Vanilla 1.12, TBC 2.4.3, and WotLK 3.3.5a.
 * Core types (0x00–0x1B) are identical in all expansions.
 * WotLK adds: ACHIEVEMENT(0x30), GUILD_ACHIEVEMENT(0x31), PARTY_LEADER(0x33).
 */
enum class ChatType : uint8_t {
    SYSTEM = 0x00,
    SAY = 0x01,
    PARTY = 0x02,
    RAID = 0x03,
    GUILD = 0x04,
    OFFICER = 0x05,
    YELL = 0x06,
    WHISPER = 0x07,
    WHISPER_FOREIGN = 0x08,
    WHISPER_INFORM = 0x09,
    EMOTE = 0x0A,
    TEXT_EMOTE = 0x0B,
    MONSTER_SAY = 0x0C,
    MONSTER_PARTY = 0x0D,
    MONSTER_YELL = 0x0E,
    MONSTER_WHISPER = 0x0F,
    MONSTER_EMOTE = 0x10,
    CHANNEL = 0x11,
    CHANNEL_JOIN = 0x12,
    CHANNEL_LEAVE = 0x13,
    CHANNEL_LIST = 0x14,
    CHANNEL_NOTICE = 0x15,
    CHANNEL_NOTICE_USER = 0x16,
    AFK = 0x17,
    DND = 0x18,
    IGNORED = 0x19,
    SKILL = 0x1A,
    LOOT = 0x1B,
    // 0x1C–0x23: the messages the game narrates rather than a player sending
    // them - loot, money, experience, honour, reputation.
    //
    // Absent, every one of these fell to the default and was named UNKNOWN, so
    // the event fired was CHAT_MSG_UNKNOWN and FrameXML's chat dropped it.
    // With the chat window handed over that is the loot line, the money line,
    // "you gain 150 experience", honour, reputation standing and the
    // tradeskill and pet notices all missing. Values from AzerothCore's
    // SharedDefines; they fill the run between LOOT and the BG block exactly.
    MONEY = 0x1C,
    OPENING = 0x1D,
    TRADESKILLS = 0x1E,
    PET_INFO = 0x1F,
    COMBAT_MISC_INFO = 0x20,
    COMBAT_XP_GAIN = 0x21,
    COMBAT_HONOR_GAIN = 0x22,
    COMBAT_FACTION_CHANGE = 0x23,
    // 0x24–0x26: BG system messages
    BG_SYSTEM_NEUTRAL  = 0x24,
    BG_SYSTEM_ALLIANCE = 0x25,
    BG_SYSTEM_HORDE    = 0x26,
    RAID_LEADER = 0x27,
    RAID_WARNING = 0x28,
    RAID_BOSS_EMOTE = 0x29,
    RAID_BOSS_WHISPER = 0x2A,
    FILTERED = 0x2B,
    BATTLEGROUND = 0x2C,
    BATTLEGROUND_LEADER = 0x2D,
    RESTRICTED = 0x2E,
    ACHIEVEMENT = 0x30,
    GUILD_ACHIEVEMENT = 0x31,
    // 0x32 is CHAT_MSG_ARENA_POINTS, and is deliberately absent rather than
    // overlooked: AzerothCore declares it in SharedDefines.h and never sends
    // it - the only other mentions are the generated enum reflection. Naming
    // it here would add a type nothing can arrive as.
    //
    // Checked while chasing CHAT_MSG_TARGETICONS, which FrameXML registers for
    // and which looks like the gap at 0x32. It is not a wire type at all in
    // 3.3.5a; the client generates that message itself when a raid marker is
    // set, so nothing on the wire is missing for it either.
    PARTY_LEADER = 0x33,
};

/**
 * Chat language IDs
 */
enum class ChatLanguage : uint32_t {
    UNIVERSAL = 0,
    ORCISH = 1,
    DARNASSIAN = 2,
    TAURAHE = 3,
    DWARVISH = 6,
    COMMON = 7,
    DEMONIC = 8,
    TITAN = 9,
    THALASSIAN = 10,
    DRACONIC = 11,
    KALIMAG = 12,
    GNOMISH = 13,
    TROLL = 14,
    GUTTERSPEAK = 33,
    DRAENEI = 35,
    ZOMBIE = 36,
    GNOMISH_BINARY = 37,
    GOBLIN_BINARY = 38,
    ADDON = 0xFFFFFFFF  // Used for addon communication
};

/**
 * SMSG_MESSAGECHAT data
 */
struct MessageChatData {
    ChatType type;
    ChatLanguage language;
    uint64_t senderGuid = 0;
    std::string senderName;
    uint64_t receiverGuid = 0;
    std::string receiverName;
    std::string message;
    std::string channelName;  // For channel messages
    uint8_t chatTag = 0;      // Player flags (AFK, DND, GM, etc.)
    uint32_t achievementId = 0; // Trailing field for ACHIEVEMENT/GUILD_ACHIEVEMENT
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    // Monotonic id assigned by ChatHandler when the message enters history.
    // Stable cache key for the chat UI's formatted/parsed line cache.
    uint64_t uid = 0;

    [[nodiscard]] bool isValid() const { return !message.empty(); }
};

// Returns true when a chat packet is addon traffic and extracts the standard
// prefix<TAB>payload envelope when present. Some legacy clients send the
// envelope with a spoken language instead of LANG_ADDON, so the envelope itself
// is also authoritative.
bool decodeAddonChatPayload(const MessageChatData& data,
                            std::string& prefix,
                            std::string& payload);

/**
 * Get human-readable string for chat type
 */
const char* getChatTypeString(ChatType type);
const char* getItemSubclassName(uint32_t itemClass, uint32_t subClass);

/**
 * SMSG_TEXT_EMOTE data
 */
struct TextEmoteData {
    uint64_t senderGuid = 0;
    uint32_t textEmoteId = 0;
    uint32_t emoteNum = 0;
    std::string targetName;

    [[nodiscard]] bool isValid() const { return senderGuid != 0; }
};

/**
 * Channel notification types
 */
enum class ChannelNotifyType : uint8_t {
    PLAYER_JOINED = 0x00,
    PLAYER_LEFT = 0x01,
    YOU_JOINED = 0x02,
    YOU_LEFT = 0x03,
    WRONG_PASSWORD = 0x04,
    NOT_MEMBER = 0x05,
    NOT_MODERATOR = 0x06,
    PASSWORD_CHANGED = 0x07,
    OWNER_CHANGED = 0x08,
    PLAYER_NOT_FOUND = 0x09,
    NOT_OWNER = 0x0A,
    CHANNEL_OWNER = 0x0B,
    MODE_CHANGE = 0x0C,
    ANNOUNCEMENTS_ON = 0x0D,
    ANNOUNCEMENTS_OFF = 0x0E,
    MODERATION_ON = 0x0F,
    MODERATION_OFF = 0x10,
    MUTED = 0x11,
    PLAYER_KICKED = 0x12,
    BANNED = 0x13,
    PLAYER_BANNED = 0x14,
    PLAYER_UNBANNED = 0x15,
    PLAYER_NOT_BANNED = 0x16,
    PLAYER_ALREADY_MEMBER = 0x17,
    INVITE = 0x18,
    INVITE_WRONG_FACTION = 0x19,
    WRONG_FACTION = 0x1A,
    INVALID_NAME = 0x1B,
    NOT_MODERATED = 0x1C,
    PLAYER_INVITED = 0x1D,
    PLAYER_INVITE_BANNED = 0x1E,
    THROTTLED = 0x1F,
    NOT_IN_AREA = 0x20,
    NOT_IN_LFG = 0x21,
};

/**
 * SMSG_CHANNEL_NOTIFY data
 */
struct ChannelNotifyData {
    ChannelNotifyType notifyType = ChannelNotifyType::YOU_JOINED;
    std::string channelName;
    uint64_t senderGuid = 0;

    [[nodiscard]] bool isValid() const { return !channelName.empty(); }
};

// ============================================================
// Server Info, Social, Logout
// ============================================================

/** SMSG_QUERY_TIME_RESPONSE data */
struct QueryTimeResponseData {
    uint32_t serverTime = 0;  // Unix timestamp
    uint32_t timeOffset = 0;  // Time until next daily reset
};

/** SMSG_PLAYED_TIME data */
struct PlayedTimeData {
    uint32_t totalTimePlayed = 0;  // Total seconds played
    uint32_t levelTimePlayed = 0;  // Seconds played at current level
    bool triggerMessage = false;   // Whether to show in chat
};

/** SMSG_FRIEND_STATUS data */
struct FriendStatusData {
    /// AzerothCore's FriendsResult, which is not a state but an *outcome*:
    /// 0 DB_ERROR, 1 LIST_FULL, 2 ONLINE, 3 OFFLINE, 4 NOT_FOUND, 5 REMOVED,
    /// 6 ADDED_ONLINE, 7 ADDED_OFFLINE, 8 ALREADY, 9 SELF, 10 ENEMY, and the
    /// ignore and mute results above those.
    uint8_t status = 0;
    uint64_t guid = 0;
    std::string note;
    /// FriendStatus, and a bitmask rather than a number: 0x01 online, 0x02
    /// away, 0x04 do-not-disturb, 0x08 recruit-a-friend. An away friend is
    /// 0x03, not 0x02.
    uint8_t chatFlag = 0;
    uint32_t areaId = 0;
    uint32_t level = 0;
    uint32_t classId = 0;
};

/** SMSG_LOGOUT_RESPONSE data */
struct LogoutResponseData {
    uint32_t result = 0;  // 0 = success, 1 = failure
    uint8_t instant = 0;  // 1 = instant logout
};

// ============================================================
// Petitions and Guild
// ============================================================

/** SMSG_PETITION_SHOWLIST data */
struct PetitionShowlistData {
    /// One charter on offer. A guild registrar offers one; an arena registrar
    /// offers three, for the two, three and five person teams.
    struct Charter {
        uint32_t index = 0;
        uint32_t itemId = 0;
        uint32_t displayId = 0;
        uint32_t cost = 0;
        uint32_t unknown = 0;
        uint32_t requiredSigs = 0;
    };
    uint64_t npcGuid = 0;
    std::vector<Charter> charters;
    // The first charter, flattened, for everything that only wants the one.
    uint32_t itemId = 0;
    uint32_t displayId = 0;
    uint32_t cost = 0;
    uint32_t charterType = 0;
    uint32_t requiredSigs = 0;

    /// Guild Charter is item 5863; the arena charters are 23560, 23561 and
    /// 23562. Which of the two panels opens depends on this and nothing else -
    /// the opcode is the same for both.
    [[nodiscard]] bool isGuildCharter() const { return itemId == 5863; }

    [[nodiscard]] bool isValid() const { return npcGuid != 0; }
};

// Guild event type constants
namespace GuildEvent {
    constexpr uint8_t PROMOTION      = 0;
    constexpr uint8_t DEMOTION       = 1;
    constexpr uint8_t MOTD           = 2;
    constexpr uint8_t JOINED         = 3;
    constexpr uint8_t LEFT           = 4;
    constexpr uint8_t REMOVED        = 5;
    constexpr uint8_t LEADER_IS      = 6;
    constexpr uint8_t LEADER_CHANGED = 7;
    constexpr uint8_t DISBANDED      = 8;
    constexpr uint8_t TABARD_CHANGED = 9;
    constexpr uint8_t RANK_UPDATED   = 10;
    constexpr uint8_t RANK_DELETED   = 11;
    constexpr uint8_t SIGNED_ON      = 12;
    constexpr uint8_t SIGNED_OFF     = 13;
    // The bank half of the event list. These are broadcast to the whole guild
    // every time anyone touches the bank, so they are frequent and none of
    // them is a chat line - they carry the numbers the bank panel reads.
    constexpr uint8_t BANK_BAG_SLOTS_CHANGED = 14;
    constexpr uint8_t BANK_TAB_PURCHASED     = 15;
    constexpr uint8_t BANK_TAB_UPDATED       = 16;
    constexpr uint8_t BANK_MONEY_SET         = 17;
    constexpr uint8_t BANK_TAB_AND_MONEY     = 18;
    constexpr uint8_t BANK_TEXT_CHANGED      = 19;
}

/// The bank balance out of a BANK_MONEY_SET event, which carries it as text.
///
/// A 3.3.5 server writes it as sixteen zero-padded hex digits, so
/// "000000000051451B" is 532g 61s 7c. Read as hex when the string says hex -
/// its length, or a digit above nine - and as decimal otherwise, so a server
/// that sends the plain number is not multiplied by sixteen.
inline uint64_t guildEventMoney(const std::string& text) {
    if (text.empty()) return 0;
    const bool hex = text.size() == 16 ||
                     text.find_first_of("abcdefABCDEFxX") != std::string::npos;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, hex ? 16 : 10);
    if (end == text.c_str()) return 0;
    return static_cast<uint64_t>(value);
}

/** SMSG_GUILD_QUERY_RESPONSE data */
struct GuildQueryResponseData {
    uint32_t guildId = 0;
    std::string guildName;
    std::string rankNames[10];
    uint32_t emblemStyle = 0;
    uint32_t emblemColor = 0;
    uint32_t borderStyle = 0;
    uint32_t borderColor = 0;
    uint32_t backgroundColor = 0;
    uint32_t rankCount = 0;

    [[nodiscard]] bool isValid() const { return guildId != 0 && !guildName.empty(); }
};

/** SMSG_GUILD_INFO data */
struct GuildInfoData {
    std::string guildName;
    uint32_t numMembers = 0;
    uint32_t numAccounts = 0;

    [[nodiscard]] bool isValid() const { return !guildName.empty(); }
};

/** Guild roster member entry */
struct GuildRosterMember {
    uint64_t guid = 0;
    bool online = false;
    std::string name;
    uint32_t rankIndex = 0;
    uint8_t level = 0;
    uint8_t classId = 0;
    uint8_t gender = 0;
    uint32_t zoneId = 0;
    float lastOnline = 0.0f;
    std::string publicNote;
    std::string officerNote;
};

/** Guild rank info */
struct GuildRankInfo {
    uint32_t rights = 0;
    uint32_t goldLimit = 0;
    // Per guild bank tab, for this rank. The roster has always carried both -
    // they were read to stay aligned with the packet and then thrown away.
    std::array<uint32_t, 6> bankTabRights{};
    std::array<uint32_t, 6> bankTabSlotsPerDay{};
};

/** SMSG_GUILD_ROSTER data */
struct GuildRosterData {
    std::string motd;
    std::string guildInfo;
    std::vector<GuildRankInfo> ranks;
    std::vector<GuildRosterMember> members;

    [[nodiscard]] bool isEmpty() const { return members.empty(); }
};

/// The two facts that separate SMSG_GUILD_ROSTER's three shapes.
///
/// Everything else about the packet is the same in all three, and it was
/// written out three times - 31, 99 and 100 lines - to say so.
struct GuildRosterLayout {
    /// A rank count on the wire, and per rank a gold withdrawal limit and six
    /// pairs of bank tab rights. Vanilla has none of that: ten ranks, rights
    /// only.
    bool rankCount;
    /// The uint8 between classId and areaId. WotLK only.
    bool gender;
};

/** SMSG_GUILD_EVENT data */
struct GuildEventData {
    uint8_t eventType = 0;
    uint8_t numStrings = 0;
    std::string strings[3];
    uint64_t guid = 0;

    [[nodiscard]] bool isValid() const { return true; }
};

/** SMSG_GUILD_INVITE data */
struct GuildInviteResponseData {
    std::string inviterName;
    std::string guildName;

    [[nodiscard]] bool isValid() const { return !inviterName.empty(); }
};

/** SMSG_GUILD_COMMAND_RESULT data */
struct GuildCommandResultData {
    uint32_t command = 0;
    std::string name;
    uint32_t errorCode = 0;

    [[nodiscard]] bool isValid() const { return true; }
};

// ============================================================
// Raid Targets, Random Roll
// ============================================================

/** Decoded MSG_RAID_TARGET_UPDATE broadcast from the server */
struct RaidTargetUpdateData {
    /// True for the full list, which is authoritative: icons missing from it
    /// are unmarked. False for a single mark being set or cleared.
    bool fullList = false;
    /// icon index (0-7) -> marked GUID, 0 meaning the icon was cleared
    std::vector<std::pair<uint8_t, uint64_t>> marks;
};

/** SMSG_RANDOM_ROLL data */
struct RandomRollData {
    uint64_t rollerGuid = 0;
    uint64_t targetGuid = 0;  // 0 for party roll
    uint32_t minRoll = 0;
    uint32_t maxRoll = 0;
    uint32_t result = 0;
};

// ============================================================
// Queries - names, creatures, game objects, pages, items
// ============================================================

/** SMSG_NAME_QUERY_RESPONSE data */
struct NameQueryResponseData {
    uint64_t guid = 0;
    uint8_t found = 1;        // 0 = found, 1 = not found
    std::string name;
    std::string realmName;
    uint8_t race = 0;
    uint8_t gender = 0;
    uint8_t classId = 0;

    [[nodiscard]] bool isValid() const { return found == 0 && !name.empty(); }
};

/** SMSG_CREATURE_QUERY_RESPONSE data */
struct CreatureQueryResponseData {
    uint32_t entry = 0;
    std::string name;
    std::string subName;
    std::string iconName;
    uint32_t typeFlags = 0;
    uint32_t creatureType = 0;  // 1=Beast, 2=Dragonkin, 3=Demon, 4=Elemental, 5=Giant, 6=Undead, 7=Humanoid, ...
    uint32_t family = 0;
    uint32_t rank = 0;         // 0=Normal, 1=Elite, 2=Rare Elite, 3=Boss, 4=Rare
    uint32_t displayId[4] = {};  // Up to 4 random display models (0 = unused)

    [[nodiscard]] bool isValid() const { return entry != 0 && !name.empty(); }
};

/** SMSG_GAMEOBJECT_QUERY_RESPONSE data */
struct GameObjectQueryResponseData {
    uint32_t entry = 0;
    std::string name;
    uint32_t type = 0;       // GameObjectType (e.g. 3=chest, 2=questgiver, 15=MO_TRANSPORT)
    uint32_t displayId = 0;
    uint32_t data[24] = {};  // Type-specific data fields (e.g. data[0]=taxiPathId for MO_TRANSPORT)
    bool hasData = false;    // Whether data[] was parsed

    [[nodiscard]] bool isValid() const { return entry != 0 && !name.empty(); }
};

/** SMSG_PAGE_TEXT_QUERY_RESPONSE data */
struct PageTextQueryResponseData {
    uint32_t pageId = 0;
    std::string text;
    uint32_t nextPageId = 0;

    [[nodiscard]] bool isValid() const { return pageId != 0; }
};

/** SMSG_ITEM_QUERY_SINGLE_RESPONSE data */
/// Whether an item query describes a bandage.
///
/// The pair is what identifies one - class alone is every consumable and
/// subclass alone means something different in every other class - so the two
/// halves are checked together, in one place, rather than in each subsystem
/// that cares. The inventory handler and the spell handler each had their own
/// copy of this and of the two numbers behind it.
struct ItemQueryResponseData;
bool isBandageItem(const ItemQueryResponseData* info);

struct ItemQueryResponseData {
    uint32_t entry = 0;
    std::string name;
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
    uint32_t displayInfoId = 0;
    uint32_t quality = 0;
    uint32_t itemFlags = 0;     // Item flag bitmask (Heroic=0x8, Unique-Equipped=0x1000000)
    uint32_t inventoryType = 0;
    int32_t maxCount = 0;       // Max that can be carried (1 = Unique, 0 = unlimited)
    int32_t maxStack = 1;
    uint32_t containerSlots = 0;
    float damageMin = 0.0f;
    float damageMax = 0.0f;
    uint32_t delayMs = 0;
    int32_t armor = 0;
    int32_t holyRes = 0;
    int32_t fireRes = 0;
    int32_t natureRes = 0;
    int32_t frostRes = 0;
    int32_t shadowRes = 0;
    int32_t arcaneRes = 0;
    int32_t stamina = 0;
    int32_t strength = 0;
    int32_t agility = 0;
    int32_t intellect = 0;
    int32_t spirit = 0;
    uint32_t sellPrice = 0;
    uint32_t itemLevel = 0;
    uint32_t requiredLevel = 0;
    std::string subclassName;
    // Item spells (up to 5)
    struct ItemSpell {
        uint32_t spellId = 0;
        uint32_t spellTrigger = 0; // 0=Use, 1=Equip, 2=ChanceOnHit, 5=Learn
    };
    std::array<ItemSpell, 5> spells{};
    uint32_t bindType = 0;      // 0=none, 1=BoP, 2=BoE, 3=BoU, 4=BoQ
    std::string description;    // Flavor/lore text
    uint32_t pageTextId = 0;     // Non-zero: item can be read via CMSG_READ_ITEM + page query
    /// Which of PageTextMaterial.dbc's seven backings the page is drawn on -
    /// Parchment, Stone, Marble, Silver, Bronze, Valentine, Illidan. Read off
    /// the same four post-description words as pageTextId and thrown away
    /// until 2026-08-06, so every letter and book opened on parchment.
    uint32_t pageMaterial = 0;
    // Generic stat pairs for non-primary stats (hit, crit, haste, AP, SP, etc.)
    struct ExtraStat { uint32_t statType = 0; int32_t statValue = 0; };
    std::vector<ExtraStat> extraStats;

    /// File one (type, value) pair from an item query into this item.
    ///
    /// Five of the ITEM_MOD ids have a field of their own here because
    /// everything reads them; the rest are kept as they came so the tooltip can
    /// name them. A pair with a zero value is a slot the item does not use -
    /// every item ships the full run of them - and is dropped rather than
    /// listed as "+0".
    ///
    /// Written out twice before this, once in the WotLK parser and once in the
    /// vanilla one, and the two did not agree. The vanilla copy wrapped the
    /// whole switch in `if (statType != 0)`, and ITEM_MOD_MANA is 0 - so a
    /// vanilla item with +Mana on it lost that line, while the same item on
    /// WotLK kept it. This is the wire's own format and does not differ by
    /// expansion; only the code did.
    /// Read the fourteen fields every expansion sends in the same order, from
    /// AllowableClass through ContainerSlots.
    ///
    /// The three item-query parsers each wrote this run out. It is the same
    /// bytes in all three - checked, not assumed - and after it they diverge
    /// for real: WotLK goes on to the scaling stats, the other two to damage.
    /// So this is the shared prefix and nothing beyond it.
    ///
    /// A run of same-width fields is the shape that misreads quietly: get the
    /// count wrong and every field after it is one place out, with each one
    /// still a plausible number. Three copies is three chances at that.
    ///
    /// False means the packet ended inside the run.
    bool readCommonRequirements(network::Packet& packet);

    void applyStat(uint32_t statType, int32_t statValue) {
        switch (statType) {
            case 3: agility = statValue; break;
            case 4: strength = statValue; break;
            case 5: intellect = statValue; break;
            case 6: spirit = statValue; break;
            case 7: stamina = statValue; break;
            default:
                if (statValue != 0) extraStats.push_back({.statType = statType, .statValue = statValue});
                break;
        }
    }
    uint32_t startQuestId = 0;  // Non-zero: item begins a quest
    // Gem socket slots (WotLK/TBC): 0=no socket; color mask: 1=Meta,2=Red,4=Yellow,8=Blue
    std::array<uint32_t, 3> socketColor{};
    // What the *template* ships in each socket. Almost always empty - a gem an
    // owner put in lives in the item's enchantment fields, not here.
    std::array<uint32_t, 3> socketContent{};
    uint32_t socketBonus = 0;   // enchantmentId of socket bonus; 0=none
    // GemProperties.dbc id: non-zero when this item is itself a gem. An item's
    // socket has a colour mask; a gem does not, and this is where its colour
    // has to be looked up from.
    uint32_t gemProperties = 0;
    uint32_t itemSetId   = 0;   // ItemSet.dbc entry; 0=not part of a set
    /// Which specialised bag this item belongs in, as a mask: 1 arrows,
    /// 2 bullets, 4 soul shards, and so on up through the profession bags.
    /// Zero for the great majority of items, which any bag will hold. On a
    /// bag it is what that bag accepts.
    uint32_t bagFamily   = 0;
    // Requirement fields
    uint32_t requiredSkill = 0;       // SkillLine.dbc ID (0 = no skill required)
    uint32_t requiredSkillRank = 0;   // Minimum skill value
    uint32_t allowableClass = 0;      // Class bitmask (0 = all classes)
    uint32_t allowableRace  = 0;      // Race bitmask (0 = all races)
    uint32_t requiredReputationFaction = 0;  // Faction.dbc ID (0 = none)
    uint32_t requiredReputationRank    = 0;  // 0=Hated..8=Exalted
    bool valid = false;
};

// ============================================================
// Combat Core
// ============================================================

/** SMSG_MONSTER_MOVE data */
struct MonsterMoveData {
    uint64_t guid = 0;
    float x = 0, y = 0, z = 0;  // Current position (server coords)
    uint8_t moveType = 0;  // 0=Normal, 1=Stop, 2=FacingSpot, 3=FacingTarget, 4=FacingAngle
    float facingAngle = 0;
    uint64_t facingTarget = 0;
    uint32_t splineFlags = 0;
    uint32_t duration = 0;
    // Destination (final point of the spline, server coords)
    float destX = 0, destY = 0, destZ = 0;
    bool hasDest = false;
    // Intermediate waypoints along the path (server coords, excludes start & final dest)
    struct Point { float x, y, z; };
    std::vector<Point> waypoints;
};

/** SMSG_ATTACKSTART data */
struct AttackStartData {
    uint64_t attackerGuid = 0;
    uint64_t victimGuid = 0;
    [[nodiscard]] bool isValid() const { return attackerGuid != 0 && victimGuid != 0; }
};

/** SMSG_ATTACKSTOP data */
struct AttackStopData {
    uint64_t attackerGuid = 0;
    uint64_t victimGuid = 0;
    uint32_t unknown = 0;
    [[nodiscard]] bool isValid() const { return true; }
};

/** Sub-damage entry for melee hits */
struct SubDamage {
    uint32_t schoolMask = 0;
    float damage = 0.0f;
    uint32_t intDamage = 0;
    uint32_t absorbed = 0;
    uint32_t resisted = 0;
};

/** SMSG_ATTACKERSTATEUPDATE data */
struct AttackerStateUpdateData {
    /// The run classic and TBC send identically: hit flags, both packed guids,
    /// the total, and the sub-damage list. What follows it is not shared -
    /// classic sends an overkill where TBC sends an unknown and a spell id -
    /// so the tails stay with their own readers.
    ///
    /// Rewinds to where it started and answers false on a short packet, so a
    /// caller can try another reader against the same bytes.
    bool readCommonHead(network::Packet& packet, size_t startPos);

    uint32_t hitInfo = 0;
    uint64_t attackerGuid = 0;
    uint64_t targetGuid = 0;
    int32_t totalDamage = 0;
    uint8_t subDamageCount = 0;
    std::vector<SubDamage> subDamages;
    uint32_t victimState = 0;    // 0=hit, 1=dodge, 2=parry, 3=interrupt, 4=block, etc.
    int32_t overkill = -1;
    uint32_t blocked = 0;

    [[nodiscard]] bool isValid() const { return attackerGuid != 0; }
    [[nodiscard]] bool isCrit() const     { return (hitInfo & 0x0200) != 0; }
    [[nodiscard]] bool isMiss() const     { return (hitInfo & 0x0010) != 0; }
    [[nodiscard]] bool isGlancing() const { return (hitInfo & 0x0800) != 0; }
    [[nodiscard]] bool isCrushing() const { return (hitInfo & 0x1000) != 0; }
};

/** SMSG_SPELLNONMELEEDAMAGELOG data (simplified) */
struct SpellDamageLogData {
    uint64_t targetGuid = 0;
    uint64_t attackerGuid = 0;
    uint32_t spellId = 0;
    uint32_t damage = 0;
    uint32_t overkill = 0;
    uint8_t schoolMask = 0;
    uint32_t absorbed = 0;
    uint32_t resisted = 0;
    bool isCrit = false;

    [[nodiscard]] bool isValid() const { return spellId != 0; }
};

/** SMSG_SPELLHEALLOG data (simplified) */
struct SpellHealLogData {
    uint64_t targetGuid = 0;
    uint64_t casterGuid = 0;
    uint32_t spellId = 0;
    uint32_t heal = 0;
    uint32_t overheal = 0;
    uint32_t absorbed = 0;
    bool isCrit = false;

    [[nodiscard]] bool isValid() const { return spellId != 0; }
};

// ============================================================
// XP Gain
// ============================================================

/** SMSG_LOG_XPGAIN data */
struct XpGainData {
    uint64_t victimGuid = 0;    // 0 for non-kill XP (quest, exploration)
    uint32_t totalXp = 0;
    uint8_t type = 0;           // 0 = kill, 1 = non-kill
    uint32_t groupBonus = 0;

    [[nodiscard]] bool isValid() const { return totalXp > 0; }
};

// ============================================================
// Spells, Action Bar, Auras
// ============================================================

/** SMSG_INITIAL_SPELLS data */
struct InitialSpellsData {
    uint8_t talentSpec = 0;
    std::vector<uint32_t> spellIds;
    std::vector<SpellCooldownEntry> cooldowns;

    [[nodiscard]] bool isValid() const { return true; }
};

/** SMSG_CAST_FAILED data */
// Normalized (WotLK-numbered) SpellCastResults whose SMSG_CAST_RESULT /
// SMSG_CAST_FAILED payload carries trailing uint32 ids the client can surface:
//  - requires-spell-focus: one SpellFocusObject id (forge, anvil, cooking fire)
//  - totems / totem-category: up to two ids naming the missing crafting tool -
//    item ids for totems, TotemCategory.dbc ids (Blacksmith Hammer, Mining
//    Pick, ...) for totem-category.
inline constexpr uint8_t kCastResultRequiresSpellFocus = 102;
inline constexpr uint8_t kCastResultTotemCategory = 130;
inline constexpr uint8_t kCastResultTotems = 131;

struct CastFailedData {
    uint8_t castCount = 0;
    uint32_t spellId = 0;
    uint8_t result = 0;
    uint32_t miscArg = 0;   // first trailing id (see readCastResultArgs)
    uint32_t miscArg2 = 0;  // second trailing id (totem failures only)

    [[nodiscard]] bool isValid() const { return spellId != 0; }
};

/** SMSG_SPELL_START data (simplified) */
struct SpellStartData {
    uint64_t casterGuid = 0;
    uint64_t casterUnit = 0;
    uint8_t castCount = 0;
    uint32_t spellId = 0;
    uint32_t castFlags = 0;
    uint32_t castTime = 0;
    uint64_t targetGuid = 0;

    [[nodiscard]] bool isValid() const { return spellId != 0; }
};

/** SMSG_SPELL_GO data (simplified) */
struct SpellGoMissEntry {
    uint64_t targetGuid = 0;
    uint8_t  missType   = 0;  // 0=MISS 1=DODGE 2=PARRY 3=BLOCK 4=EVADE 5=IMMUNE 6=DEFLECT 7=ABSORB 8=RESIST 11=REFLECT
};

struct SpellGoData {
    uint64_t casterGuid = 0;
    uint64_t casterUnit = 0;
    uint8_t castCount = 0;
    uint32_t spellId = 0;
    uint32_t castFlags = 0;
    uint8_t hitCount = 0;
    std::vector<uint64_t>       hitTargets;
    uint8_t missCount = 0;
    std::vector<SpellGoMissEntry> missTargets;
    uint64_t targetGuid = 0;  ///< Primary target GUID from SpellCastTargets (0 = none/AoE)

    [[nodiscard]] bool isValid() const { return spellId != 0; }
};

/** SMSG_AURA_UPDATE / SMSG_AURA_UPDATE_ALL data */
struct AuraUpdateData {
    uint64_t guid = 0;
    std::vector<std::pair<uint8_t, AuraSlot>> updates; // slot index + aura data

    [[nodiscard]] bool isValid() const { return true; }
};

/** SMSG_SPELL_COOLDOWN data */
struct SpellCooldownData {
    uint64_t guid = 0;
    uint8_t flags = 0;
    std::vector<std::pair<uint32_t, uint32_t>> cooldowns; // spellId, cooldownMs

    [[nodiscard]] bool isValid() const { return true; }
};

// ============================================================
// Group/Party System
// ============================================================

/** SMSG_GROUP_INVITE data */
struct GroupInviteResponseData {
    uint8_t canAccept = 0;
    std::string inviterName;

    [[nodiscard]] bool isValid() const { return !inviterName.empty(); }
};

/** SMSG_PARTY_COMMAND_RESULT data */
struct PartyCommandResultData {
    PartyCommand command;
    std::string name;
    PartyResult result;

    [[nodiscard]] bool isValid() const { return true; }
};

/** SMSG_GROUP_DECLINE data */
struct GroupDeclineData {
    std::string playerName;
    [[nodiscard]] bool isValid() const { return !playerName.empty(); }
};

// ============================================================
// Loot System
// ============================================================

/** Loot item entry */
struct LootItem {
    uint8_t slotIndex = 0;
    uint32_t itemId = 0;
    uint32_t count = 0;
    uint32_t displayInfoId = 0;
    uint32_t randomSuffix = 0;
    uint32_t randomPropertyId = 0;
    uint8_t lootSlotType = 0;
    bool isQuestItem = false;
    bool looted = false;
};

/** SMSG_LOOT_RESPONSE data */
struct LootResponseData {
    uint64_t lootGuid = 0;
    uint8_t lootType = 0;
    uint32_t gold = 0;           // In copper
    bool goldLooted = false;     // Taken; the coin keeps its slot
    std::vector<LootItem> items;

    /// Whether display slot one is the coin, taken or not.
    [[nodiscard]] bool hasCoinSlot() const { return gold > 0; }
    [[nodiscard]] bool goldLeft() const { return gold > 0 && !goldLooted; }

    [[nodiscard]] bool nothingLeft() const {
        if (goldLeft()) return false;
        for (const auto& item : items)
            if (!item.looted) return false;
        return true;
    }

    [[nodiscard]] bool isValid() const { return true; }
    [[nodiscard]] uint32_t getGold() const { return gold / 10000; }
    [[nodiscard]] uint32_t getSilver() const { return (gold / 100) % 100; }
    [[nodiscard]] uint32_t getCopper() const { return gold % 100; }
};

// ============================================================
// NPC Gossip
// ============================================================

/** Gossip menu option */
struct GossipOption {
    uint32_t id = 0;
    uint8_t icon = 0;          // 0=chat, 1=vendor, 2=taxi, 3=trainer, etc.
    bool isCoded = false;
    uint32_t boxMoney = 0;
    std::string text;
    std::string boxText;
};

/** Gossip quest item */
struct GossipQuestItem {
    uint32_t questId = 0;
    uint32_t questIcon = 0;
    int32_t questLevel = 0;
    uint32_t questFlags = 0;
    uint8_t isRepeatable = 0;
    std::string title;
};

/** SMSG_GOSSIP_MESSAGE data */
struct GossipMessageData {
    uint64_t npcGuid = 0;
    uint32_t menuId = 0;
    uint32_t titleTextId = 0;
    std::vector<GossipOption> options;
    std::vector<GossipQuestItem> quests;

    [[nodiscard]] bool isValid() const { return true; }
};

// ============================================================
// Bind Point (Hearthstone)
// ============================================================

struct BindPointUpdateData {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t mapId = 0;
    uint32_t zoneId = 0;
};

// ============================================================
// Quests
// ============================================================

/** Reward item entry (shared by quest detail/offer windows) */
struct QuestRewardItem {
    uint32_t itemId = 0;
    uint32_t count = 0;
    uint32_t displayInfoId = 0;
    uint32_t choiceSlot = 0;  // Original reward slot index from server payload
};

/** SMSG_QUESTGIVER_QUEST_DETAILS data (simplified) */
struct QuestDetailsData {
    uint64_t npcGuid = 0;
    uint32_t questId = 0;
    std::string title;
    std::string details;      // Quest description text
    std::string objectives;   // Objectives text
    uint32_t suggestedPlayers = 0;
    /// QUEST_FLAGS_* as AzerothCore defines them. Only 3.x carries this - the
    /// TBC and Classic offers have no flags field at all, so it stays zero
    /// there rather than being guessed at.
    uint32_t questFlags = 0;
    uint32_t rewardMoney = 0;
    uint32_t rewardXp = 0;
    std::vector<QuestRewardItem> rewardChoiceItems;  // Player picks one of these
    std::vector<QuestRewardItem> rewardItems;         // These are always given
    /// The spell shown under "You will learn:" on the offer. Sent three fields
    /// past the XP - honor, an unused float, then this.
    ///
    /// The cast id and the title id follow it on the wire and are deliberately
    /// not kept: nothing reads them, and a field stored for nobody is a claim
    /// that something uses it.
    uint32_t rewardSpellId = 0;
};

/** SMSG_QUESTGIVER_REQUEST_ITEMS data (turn-in progress check) */
struct QuestRequestItemsData {
    uint64_t npcGuid = 0;
    uint32_t questId = 0;
    std::string title;
    std::string completionText;
    uint32_t requiredMoney = 0;
    uint32_t completableFlags = 0;  // 0x03 = completable
    std::vector<QuestRewardItem> requiredItems;

    [[nodiscard]] bool isCompletable() const { return (completableFlags & 0x03) != 0; }
};

/** SMSG_QUESTGIVER_OFFER_REWARD data (choose reward) */
struct QuestOfferRewardData {
    uint64_t npcGuid = 0;
    uint32_t questId = 0;
    std::string title;
    std::string rewardText;
    uint32_t rewardMoney = 0;
    uint32_t rewardXp = 0;
    uint32_t rewardHonor = 0;   // honor points (the packet's ×10 value, unscaled)
    uint32_t rewardTalents = 0;
    uint32_t rewardArenaPoints = 0;
    uint32_t rewardTitleId = 0;  // CharTitles.dbc id of an awarded title, 0 = none
    struct FactionReward { uint32_t factionId = 0; int32_t valueId = 0; int32_t override = 0; };
    std::array<FactionReward, 5> factionRewards{};
    std::vector<QuestRewardItem> choiceRewards;  // Pick one
    std::vector<QuestRewardItem> fixedRewards;   // Always given
};

/** Protocol era for quest packets whose layout differs per expansion. */
enum class QuestPacketEra { CLASSIC, TBC, WOTLK };

/** Reward block extracted from SMSG_QUEST_QUERY_RESPONSE (quest log rewards). */
struct QuestQueryRewardsData {
    int32_t  rewardMoney = 0;
    std::array<uint32_t, 4> itemId{};
    std::array<uint32_t, 4> itemCount{};
    std::array<uint32_t, 6> choiceItemId{};
    std::array<uint32_t, 6> choiceItemCount{};
    /// The quest's start item, zero when it has none or when the layout for
    /// this expansion has not been verified against a server serializer.
    uint32_t sourceItemId = 0;
    /// The spell the quest shows as a reward - a recipe taught, a buff given.
    /// Same caveat as the start item: WotLK only, because that is the layout
    /// counted off a serializer here.
    uint32_t rewardSpellId = 0;
    /// The quest's XP-difficulty index (GetXPId), which selects a column of
    /// QuestXP.dbc; the reward experience is that column read at the quest's
    /// level. Zero on the layouts not counted off a serializer here (classic,
    /// TBC), which keeps their reward XP absent rather than guessed.
    uint32_t xpId = 0;
    /// Honor, bonus talent points and arena points a quest awards - direct
    /// values in the query response (RewHonorAddition, BonusTalents,
    /// RewArenaPoints). WotLK only, same caveat as the fields above; honor is
    /// the flat addition, which is what most quests set (the multiplier path is
    /// not derived here).
    uint32_t rewardHonor = 0;
    uint32_t bonusTalents = 0;
    uint32_t arenaPoints = 0;
    /// CharTitles.dbc id of a title the quest awards (few quests do). WotLK only.
    uint32_t rewardTitleId = 0;
    /// Up to five reputation rewards: the faction id, the QuestFactionReward.dbc
    /// value index, and a direct override (hundredths). The reputation shown is
    /// override/100 when set, else the dbc column at |valueId|. WotLK only.
    std::array<uint32_t, 5> factionId{};
    std::array<int32_t, 5>  factionValueId{};
    std::array<int32_t, 5>  factionValueOverride{};
    bool valid = false;
};

// ============================================================
// Vendor
// ============================================================

/** Vendor item entry */
struct VendorItem {
    uint32_t slot = 0;
    uint32_t itemId = 0;
    uint32_t displayInfoId = 0;
    int32_t maxCount = -1;       // -1 = unlimited
    uint32_t buyPrice = 0;       // In copper
    uint32_t durability = 0;
    uint32_t stackCount = 0;
    uint32_t extendedCost = 0;
};

/** SMSG_LIST_INVENTORY data */
struct ListInventoryData {
    uint64_t vendorGuid = 0;
    std::vector<VendorItem> items;
    bool canRepair = false;  // Set when vendor was opened via GOSSIP_OPTION_ARMORER

    [[nodiscard]] bool isValid() const { return true; }
};

// ============================================================
// Trainer
// ============================================================

struct TrainerSpell {
    uint32_t spellId = 0;
    uint8_t state = 0;         // 0=available(green), 1=unavailable(grey), 2=known
    uint32_t spellCost = 0;    // copper
    uint32_t profDialog = 0;
    uint32_t profButton = 0;
    uint8_t reqLevel = 0;
    uint32_t reqSkill = 0;
    uint32_t reqSkillValue = 0;
    uint32_t chainNode1 = 0;
    uint32_t chainNode2 = 0;
    uint32_t chainNode3 = 0;
};

struct TrainerListData {
    uint64_t trainerGuid = 0;
    uint32_t trainerType = 0;  // 0=class, 1=mounts, 2=tradeskills, 3=pets
    std::vector<TrainerSpell> spells;
    std::string greeting;
    [[nodiscard]] bool isValid() const { return true; }
};

/** One queue slot's worth of SMSG_BATTLEFIELD_STATUS. */
struct BattlefieldStatusData {
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

// ============================================================
// Taxi / Flight Paths
// ============================================================

static constexpr uint32_t TLK_TAXI_MASK_SIZE = 12;

/** SMSG_SHOWTAXINODES data */
struct ShowTaxiNodesData {
    uint32_t windowInfo = 0;        // 1 = show window
    uint64_t npcGuid = 0;
    uint32_t nearestNode = 0;       // Taxi node player is at
    uint32_t nodeMask[TLK_TAXI_MASK_SIZE] = {};
    [[nodiscard]] bool isNodeKnown(uint32_t nodeId) const {
        if (nodeId == 0) return false;
        uint32_t bitIndex = nodeId - 1;
        uint32_t idx = bitIndex / 32;
        uint32_t bit = bitIndex % 32;
        return idx < TLK_TAXI_MASK_SIZE && (nodeMask[idx] & (1u << bit));
    }
};

/** SMSG_ACTIVATETAXIREPLY data */
struct ActivateTaxiReplyData {
    uint32_t result = 0; // 0 = OK
};

// ============================================================
// Mail System
// ============================================================

struct MailAttachment {
    uint8_t slot = 0;
    uint32_t itemGuidLow = 0;
    uint32_t itemId = 0;
    uint32_t enchantId = 0;
    uint32_t randomPropertyId = 0;
    uint32_t randomSuffix = 0;
    uint32_t stackCount = 1;
    uint32_t chargesOrDurability = 0;
    uint32_t maxDurability = 0;
};

struct MailMessage {
    uint32_t messageId = 0;
    uint8_t messageType = 0;    // 0=normal, 2=auction, 3=creature, 4=gameobject
    uint64_t senderGuid = 0;
    uint32_t senderEntry = 0;   // For non-player mail
    std::string senderName;
    std::string subject;
    std::string body;
    uint32_t stationeryId = 0;
    uint64_t money = 0;
    uint64_t cod = 0;           // Cash on delivery
    uint32_t flags = 0;
    float expirationTime = 0.0f;
    uint32_t mailTemplateId = 0;
    bool read = false;
    std::vector<MailAttachment> attachments;
};

struct AuctionMailSubject {
    uint32_t itemEntry = 0;
    uint32_t response = 0;
    uint32_t lotId = 0;
    uint32_t itemCount = 0;
};

// Decoded auction-house mail body ("invoice"). The wire body is a colon-
// separated string the retail client never prints verbatim.
struct AuctionMailInvoice {
    uint32_t ownerGuidLow = 0;  // seller/buyer low GUID (hex field)
    uint32_t bid = 0;           // copper
    uint32_t buyout = 0;        // copper
    uint32_t deposit = 0;       // copper (successful-sale mail only)
    uint32_t consignment = 0;   // copper - auction house cut (successful sale)
};

bool parseAuctionMailSubject(const std::string& subject, AuctionMailSubject& result);
std::string formatAuctionMailSubject(const AuctionMailSubject& subject,
                                     const std::string& itemName);
bool parseAuctionMailBody(const std::string& body, AuctionMailInvoice& result);

// ============================================================
// Guild Bank System
// ============================================================

struct GuildBankItemSlot {
    uint8_t slotId = 0;
    uint32_t itemEntry = 0;
    uint32_t stackCount = 1;
    uint32_t enchantId = 0;
    uint32_t randomPropertyId = 0;
};

struct GuildBankTab {
    std::string tabName;
    std::string tabIcon;
    std::vector<GuildBankItemSlot> items;
};

struct GuildBankData {
    uint64_t money = 0;
    uint8_t tabId = 0;
    int32_t withdrawAmount = -1;  // -1 = unlimited
    std::vector<GuildBankTab> tabs;  // Only populated on fullUpdate
    std::vector<GuildBankItemSlot> tabItems;  // Current tab items
    /// Whether the list just parsed carried the tab names, which is what the
    /// panel's GUILDBANK_UPDATE_TABS means. A slots-only update leaves `tabs`
    /// as it found them, so their contents cannot answer this.
    bool tabsIncluded = false;
};

// ============================================================
// Auction House System
// ============================================================

struct AuctionEntry {
    uint32_t auctionId = 0;
    uint32_t itemEntry = 0;
    uint32_t stackCount = 1;
    uint32_t enchantId = 0;
    uint32_t randomPropertyId = 0;
    uint32_t suffixFactor = 0;
    uint64_t ownerGuid = 0;
    uint32_t startBid = 0;
    uint32_t minBidIncrement = 0;
    uint32_t buyoutPrice = 0;
    uint32_t timeLeftMs = 0;
    uint64_t bidderGuid = 0;
    uint32_t currentBid = 0;
};

struct AuctionListResult {
    std::vector<AuctionEntry> auctions;
    uint32_t totalCount = 0;
    uint32_t searchDelay = 0;
};

struct AuctionCommandResult {
    uint32_t auctionId = 0;
    uint32_t action = 0;    // 0=create, 1=cancel, 2=bid, 3=buyout
    uint32_t errorCode = 0; // 0=success
    uint32_t bidError = 0;  // secondary error for bid actions
};

struct AuctionHelloData {
    uint64_t auctioneerGuid = 0;
    uint8_t enabled = 1;
};

/** One column of an auction search's ordering, as the wire carries it.
 *
 * `column` is AzerothCore's AuctionSortOrder - MINLEVEL 0, RARITY 1, BUYOUT 2,
 * TIMELEFT 3, ITEM 5, MINBIDBUY 6, OWNER 7, BID 8, STACK 9.
 *
 * `descending` goes across as sent. The server's comparator is not uniformly
 * ordered - it answers -1 for the *higher* required level and -1 for the
 * *lower* quality - and the original interface's per-column reverse flags were
 * written against those inversions. Normalising the flag here would undo that
 * pairing and sort half the columns backwards.
 */
struct AuctionSortKey {
    uint8_t column = 0;
    bool descending = false;
};

} // namespace game
} // namespace wowee
