#pragma once

#include "guild/domain/records.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::guild {

/// The player's guild as the interface and the addon layer use it: what is
/// known about it, and requests to the server. The server's answers arrive as
/// events, not as return values.
class IGuild {
public:
    virtual ~IGuild() = default;

    // ---- Reading ----
    [[nodiscard]] virtual bool isInGuild() const = 0;
    [[nodiscard]] virtual const std::string& getGuildName() const = 0;
    [[nodiscard]] virtual const std::vector<std::string>& getGuildRankNames() const = 0;
    [[nodiscard]] virtual const Roster& getGuildRoster() const = 0;
    [[nodiscard]] virtual bool hasGuildRoster() const = 0;
    [[nodiscard]] virtual const Summary& getGuildInfoData() const = 0;
    [[nodiscard]] virtual bool hasGuildInfoData() const = 0;
    [[nodiscard]] virtual const Identity& getGuildQueryData() const = 0;
    [[nodiscard]] virtual bool hasPendingGuildInvite() const = 0;
    [[nodiscard]] virtual const std::string& getPendingGuildInviterName() const = 0;
    [[nodiscard]] virtual const std::string& getPendingGuildInviteGuildName() const = 0;
    /// The player's index into the rank list, or 0xFFFFFFFF when unknown.
    [[nodiscard]] virtual uint32_t getPlayerGuildRankIndex() const = 0;
    [[nodiscard]] virtual uint32_t getPlayerGuildRankRights() const = 0;
    [[nodiscard]] virtual const std::vector<EventLogEntry>& getGuildEventLog() const = 0;
    /// Another guild's name for a nameplate: empty until the server answers
    /// the query this starts.
    virtual const std::string& lookupGuildName(uint32_t guildId) = 0;

    // ---- Requests ----
    virtual void requestGuildInfo() = 0;
    virtual void requestGuildRoster() = 0;
    virtual void requestGuildEventLog() = 0;
    /// The longer "guild information" text, a different field from the
    /// message of the day with an opcode of its own.
    virtual void setGuildInfoText(const std::string& text) = 0;
    virtual void setGuildMotd(const std::string& motd) = 0;
    virtual void promoteGuildMember(const std::string& name) = 0;
    virtual void demoteGuildMember(const std::string& name) = 0;
    virtual void leaveGuild() = 0;
    virtual void inviteToGuild(const std::string& name) = 0;
    virtual void kickGuildMember(const std::string& name) = 0;
    virtual void disbandGuild() = 0;
    virtual void setGuildLeader(const std::string& name) = 0;
    virtual void setGuildPublicNote(const std::string& name, const std::string& note) = 0;
    virtual void setGuildOfficerNote(const std::string& name, const std::string& note) = 0;
    virtual void acceptGuildInvite() = 0;
    virtual void declineGuildInvite() = 0;
    virtual void createGuild(const std::string& guildName) = 0;
    virtual void addGuildRank(const std::string& rankName) = 0;
    /// Removes the lowest rank. The request takes no index: that is what the
    /// server does with it.
    virtual void delGuildRank() = 0;

    // ---- The guild control panel's rank editor ----
    virtual void setSelectedGuildRank(int index) = 0;
    [[nodiscard]] virtual int getSelectedGuildRank() const = 0;
    virtual RankDraft& pendingGuildRankRef() = 0;
    /// Sends the selected rank with the draft's rights under `rankName`.
    virtual void saveGuildRank(const std::string& rankName) = 0;
};

/// `guild`, or the guild of a client with no game: never a member, and every
/// request dropped - for a reader that would otherwise test for it at every
/// call.
[[nodiscard]] IGuild& orNoGuild(IGuild* guild);

/// What the protocol layer tells the guild, decoded from whichever
/// expansion's packets.
class IGuildServerFeed {
public:
    virtual ~IGuildServerFeed() = default;

    virtual void onSummary(Summary summary) = 0;
    virtual void onRoster(Roster roster) = 0;
    virtual void onIdentity(Identity identity) = 0;
    virtual void onEvent(const ServerEvent& event) = 0;
    virtual void onInvite(std::string inviter, std::string guildName) = 0;
    virtual void onInviteDeclined(std::string name) = 0;
    virtual void onCommandResult(uint32_t command, uint32_t errorCode, const std::string& name) = 0;
    virtual void onEventLog(std::vector<EventLogEntry> entries) = 0;
    /// A character is logging in: none of the last one's guild is theirs.
    virtual void beginCharacter() = 0;
};

} // namespace wowee::guild
