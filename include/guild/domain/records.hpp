#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace wowee::guild {

/// A guild's name, rank names and tabard, as its own query describes it.
struct Identity {
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

/// The guild's headline figures (SMSG_GUILD_INFO).
struct Summary {
    std::string guildName;
    uint32_t numMembers = 0;
    uint32_t numAccounts = 0;

    [[nodiscard]] bool isValid() const { return !guildName.empty(); }
};

struct RosterMember {
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

/// What a rank may do. Rank zero is the guild master.
struct Rank {
    uint32_t rights = 0;
    uint32_t goldLimit = 0;
    std::array<uint32_t, 6> bankTabRights{};
    std::array<uint32_t, 6> bankTabSlotsPerDay{};
};

struct Roster {
    std::string motd;
    std::string guildInfo;
    std::vector<Rank> ranks;
    std::vector<RosterMember> members;

    [[nodiscard]] bool isEmpty() const { return members.empty(); }
};

/// One line of the guild's event log. Types are AzerothCore's
/// GuildEventLogTypes: 1 invite, 2 join, 3 promote, 4 demote, 5 remove,
/// 6 leave. `secondsAgo` is relative to when the log was sent.
struct EventLogEntry {
    uint8_t  type = 0;
    uint64_t playerGuid = 0;
    uint64_t otherGuid = 0;      // 0 when the type carries none
    uint8_t  newRank = 0;        // only meaningful for promote/demote
    uint32_t secondsAgo = 0;
};

/// AzerothCore's GuildEvents, the type of an SMSG_GUILD_EVENT.
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
    // The bank half of the list. These are broadcast to the whole guild every
    // time anyone touches the bank, so they are frequent and none of them is
    // a chat line - they carry the numbers the bank panel reads.
    constexpr uint8_t BANK_BAG_SLOTS_CHANGED = 14;
    constexpr uint8_t BANK_TAB_PURCHASED     = 15;
    constexpr uint8_t BANK_TAB_UPDATED       = 16;
    constexpr uint8_t BANK_MONEY_SET         = 17;
    constexpr uint8_t BANK_TAB_AND_MONEY     = 18;
    constexpr uint8_t BANK_TEXT_CHANGED      = 19;
}

/// An SMSG_GUILD_EVENT, decoded: its type and up to three strings.
struct ServerEvent {
    uint8_t type = 0;
    uint8_t numStrings = 0;
    std::string strings[3];
};

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

/// A rank being edited in the guild control panel. CMSG_GUILD_RANK rewrites
/// the rank whole, so the draft starts from the rank's current values: a field
/// left at zero is not "unchanged", it is revoked.
struct RankDraft {
    uint32_t rights = 0;
    uint32_t goldLimit = 0;
    std::array<uint32_t, 6> tabRights{};
    std::array<uint32_t, 6> tabSlots{};
};

} // namespace wowee::guild
