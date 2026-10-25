#pragma once

#include "guild/domain/events.hpp"
#include "guild/domain/records.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::guild {

/// The guild of the character in the world, as far as the server has told it.
///
/// Membership has three sources, newest first: a roster this session (only
/// sent to members), the name the guild is known by, and the player's own
/// guild id. Every change comes from the server, or from the player answering
/// an invite, and returns what happened for the service to publish.
class Guild {
public:
    static constexpr uint32_t kNoRank = 0xFFFFFFFFu;

    // ---- What the server sent ----
    std::vector<GuildChange> summaryReceived(Summary summary, uint32_t ownGuildId);
    std::vector<GuildChange> rosterReceived(Roster roster);
    std::vector<GuildChange> identityReceived(Identity identity, uint32_t ownGuildId);
    std::vector<GuildChange> eventReceived(const ServerEvent& event);
    std::vector<GuildChange> inviteReceived(std::string inviter, std::string guildName);
    /// SMSG_GUILD_COMMAND_RESULT. Command 0 is create, 1 invite, 2 quit.
    std::vector<GuildChange> commandResult(uint32_t command, uint32_t errorCode,
                                           const std::string& name);
    std::vector<GuildChange> eventLogReceived(std::vector<EventLogEntry> entries);

    // ---- What the player decided ----
    /// Keeps the invite's guild name until the guild names itself: the roster
    /// that confirms the join carries no name.
    void inviteAccepted();
    void inviteDeclined();
    /// A character is logging in: none of the last one's guild is theirs.
    void beginCharacter() { *this = Guild{}; }

    // ---- The guild control panel's rank editor ----
    /// Selects a rank (one-based, as the panel counts) and starts the draft
    /// from its current values.
    void selectRank(int index);
    [[nodiscard]] int selectedRank() const { return selectedRank_; }
    [[nodiscard]] const RankDraft& rankDraft() const { return draft_; }
    RankDraft& rankDraft() { return draft_; }

    // ---- Reading it ----
    [[nodiscard]] bool isMember(uint32_t ownGuildId) const;
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::vector<std::string>& rankNames() const { return rankNames_; }
    [[nodiscard]] const Roster& roster() const { return roster_; }
    [[nodiscard]] bool hasRoster() const { return hasRoster_; }
    [[nodiscard]] const Summary& summary() const { return summary_; }
    [[nodiscard]] const Identity& identity() const { return identity_; }
    [[nodiscard]] bool hasPendingInvite() const { return invitePending_; }
    [[nodiscard]] const std::string& inviterName() const { return inviter_; }
    [[nodiscard]] const std::string& inviteGuildName() const { return inviteGuild_; }
    [[nodiscard]] const std::vector<EventLogEntry>& eventLog() const { return eventLog_; }
    /// The player's index into the rank list, or kNoRank without a roster
    /// that lists them.
    [[nodiscard]] uint32_t rankIndexOf(uint64_t playerGuid) const;
    /// The rights of the player's rank, or 0 when the rank is not known.
    [[nodiscard]] uint32_t rightsOf(uint64_t playerGuid) const;

private:
    void dropMembership();
    /// Whether a member's online state actually changes. A member the roster
    /// does not list yet counts as a change.
    bool onlineTransition(const std::string& member, bool nowOnline);

    bool confirmed_ = false;          // a roster arrived this session
    std::string joinedName_;          // the invite's name, until the guild names itself
    std::string name_;
    std::vector<std::string> rankNames_;
    Roster roster_;
    bool hasRoster_ = false;
    Summary summary_;
    Identity identity_;
    bool invitePending_ = false;
    std::string inviter_;
    std::string inviteGuild_;
    std::vector<EventLogEntry> eventLog_;
    int selectedRank_ = 1;
    RankDraft draft_;
};

} // namespace wowee::guild
