#include "guild/domain/guild.hpp"

#include <cstdlib>

namespace wowee::guild {

void Guild::dropMembership() {
    confirmed_ = false;
    joinedName_.clear();
    name_.clear();
    rankNames_.clear();
    roster_ = Roster{};
    hasRoster_ = false;
}

std::vector<GuildChange> Guild::summaryReceived(Summary summary, uint32_t ownGuildId) {
    std::vector<GuildChange> changes;
    if (!summary.guildName.empty() && ownGuildId != 0) {
        changes.emplace_back(GuildNameLearned{ownGuildId, summary.guildName});
        if (name_.empty()) name_ = summary.guildName;
    }
    // SMSG_GUILD_INFO comes with every roster sync, so only a change the
    // player can see is news.
    const bool changed = summary_.guildName != summary.guildName ||
                         summary_.numMembers != summary.numMembers ||
                         summary_.numAccounts != summary.numAccounts;
    summary_ = std::move(summary);
    if (changed)
        changes.emplace_back(SummaryChanged{summary_.guildName, summary_.numMembers, summary_.numAccounts});
    return changes;
}

std::vector<GuildChange> Guild::rosterReceived(Roster roster) {
    roster_ = std::move(roster);
    hasRoster_ = true;
    // A roster is only sent to a member, so this is the server saying so -
    // including for a guild joined since login, which no other record shows.
    const bool newlyIn = !confirmed_;
    confirmed_ = true;
    if (name_.empty() && !joinedName_.empty()) name_ = joinedName_;

    std::vector<GuildChange> changes;
    if (newlyIn) changes.emplace_back(MembershipChanged{});
    // The roster in hand is the fresh one, so nothing should ask again.
    changes.emplace_back(RosterChanged{false});
    return changes;
}

std::vector<GuildChange> Guild::identityReceived(Identity identity, uint32_t ownGuildId) {
    std::vector<GuildChange> changes;
    if (identity.guildId != 0 && !identity.guildName.empty())
        changes.emplace_back(GuildNameLearned{identity.guildId, identity.guildName});
    if (ownGuildId == 0 || ownGuildId != identity.guildId) return changes;

    const bool wasUnknown = name_.empty();
    name_ = identity.guildName;
    rankNames_.assign(std::begin(identity.rankNames), std::end(identity.rankNames));
    identity_ = std::move(identity);
    if (wasUnknown && !name_.empty()) {
        changes.emplace_back(GuildNamed{name_});
        changes.emplace_back(MembershipChanged{});
    }
    return changes;
}

bool Guild::onlineTransition(const std::string& member, bool nowOnline) {
    for (auto& m : roster_.members) {
        if (m.name != member) continue;
        if (m.online == nowOnline) return false;
        m.online = nowOnline;
        return true;
    }
    return true;
}

std::vector<GuildChange> Guild::eventReceived(const ServerEvent& e) {
    std::vector<GuildChange> changes;
    const auto& s = e.strings;
    const uint8_t n = e.numStrings;

    switch (e.type) {
        case GuildEvent::PROMOTION:
            if (n >= 3) changes.emplace_back(MemberPromoted{s[0], s[1], s[2]});
            break;
        case GuildEvent::DEMOTION:
            if (n >= 3) changes.emplace_back(MemberDemoted{s[0], s[1], s[2]});
            break;
        case GuildEvent::MOTD:
            changes.emplace_back(MotdChanged{n >= 1 ? s[0] : std::string()});
            break;
        case GuildEvent::JOINED:
            if (n >= 1) changes.emplace_back(MemberJoined{s[0]});
            break;
        case GuildEvent::LEFT:
            if (n >= 1) changes.emplace_back(MemberLeft{s[0]});
            break;
        case GuildEvent::REMOVED:
            // The member removed, then who removed them.
            if (n >= 2) changes.emplace_back(MemberRemoved{s[0], s[1]});
            break;
        case GuildEvent::LEADER_IS:
            if (n >= 1) changes.emplace_back(LeaderIs{s[0]});
            break;
        case GuildEvent::LEADER_CHANGED:
            if (n >= 2) changes.emplace_back(LeaderChanged{s[0], s[1]});
            break;
        case GuildEvent::DISBANDED:
            // Membership first: the interface rechecks IsInGuild before the
            // line about it.
            dropMembership();
            changes.emplace_back(MembershipChanged{});
            changes.emplace_back(GuildDisbanded{});
            break;
        case GuildEvent::SIGNED_ON:
            // On login the server signs on every member the roster already
            // lists as online; only a real transition is news.
            if (n >= 1 && onlineTransition(s[0], true)) changes.emplace_back(MemberCameOnline{s[0]});
            break;
        case GuildEvent::SIGNED_OFF:
            if (n >= 1 && onlineTransition(s[0], false)) changes.emplace_back(MemberWentOffline{s[0]});
            break;
        case GuildEvent::BANK_MONEY_SET:
            if (n >= 1) changes.emplace_back(BankMoneyChanged{guildEventMoney(s[0])});
            break;
        case GuildEvent::BANK_TAB_AND_MONEY:
            if (n >= 1) changes.emplace_back(BankMoneyChanged{guildEventMoney(s[0])});
            changes.emplace_back(BankTabsChanged{});
            break;
        case GuildEvent::BANK_TAB_PURCHASED:
        case GuildEvent::BANK_TAB_UPDATED:
            changes.emplace_back(BankTabsChanged{});
            break;
        case GuildEvent::BANK_BAG_SLOTS_CHANGED:
            changes.emplace_back(BankBagSlotsChanged{});
            break;
        case GuildEvent::BANK_TEXT_CHANGED:
            // The server counts tabs from zero, the interface from one.
            if (n >= 1) changes.emplace_back(BankTextChanged{static_cast<int>(std::strtol(s[0].c_str(), nullptr, 10)) + 1});
            break;
        case GuildEvent::TABARD_CHANGED:
            changes.emplace_back(TabardChanged{});
            break;
        case GuildEvent::RANK_UPDATED:
        case GuildEvent::RANK_DELETED:
            break;
        default:
            changes.emplace_back(UnknownServerEvent{e.type, n, n >= 1 ? s[0] : std::string()});
            break;
    }

    // The server sends the event, not a new roster. When the client holds a
    // roster and the event changes who is in it or where, the client asks for
    // a fresh one itself - and tells the interface not to ask a second time.
    const bool clientWillRequest = hasRoster_ && (
        e.type == GuildEvent::PROMOTION || e.type == GuildEvent::DEMOTION ||
        e.type == GuildEvent::JOINED || e.type == GuildEvent::LEFT ||
        e.type == GuildEvent::REMOVED || e.type == GuildEvent::LEADER_CHANGED ||
        // Ranks only reach the client in the roster.
        e.type == GuildEvent::RANK_UPDATED || e.type == GuildEvent::RANK_DELETED);

    switch (e.type) {
        case GuildEvent::SIGNED_ON: case GuildEvent::SIGNED_OFF:
        case GuildEvent::PROMOTION: case GuildEvent::DEMOTION:
        case GuildEvent::JOINED: case GuildEvent::LEFT:
        case GuildEvent::REMOVED: case GuildEvent::LEADER_CHANGED:
        case GuildEvent::DISBANDED:
        case GuildEvent::RANK_UPDATED: case GuildEvent::RANK_DELETED:
            changes.emplace_back(RosterChanged{!clientWillRequest});
            break;
        default:
            break;
    }
    if (clientWillRequest) changes.emplace_back(RosterOutdated{});
    return changes;
}

std::vector<GuildChange> Guild::inviteReceived(std::string inviter, std::string guildName) {
    invitePending_ = true;
    inviter_ = std::move(inviter);
    inviteGuild_ = std::move(guildName);
    return {InviteReceived{inviter_, inviteGuild_}};
}

std::vector<GuildChange> Guild::commandResult(uint32_t command, uint32_t errorCode,
                                              const std::string& name) {
    if (errorCode != 0) return {CommandFailed{errorCode, name}};
    switch (command) {
        case 0:
            return {GuildCreated{}, MembershipChanged{}, RosterChanged{false}};
        case 1:
            if (!name.empty()) return {GuildInviteSent{name}};
            return {};
        case 2:
            dropMembership();
            return {LeftGuild{}, MembershipChanged{}, RosterChanged{false}};
        default:
            return {};
    }
}

std::vector<GuildChange> Guild::eventLogReceived(std::vector<EventLogEntry> entries) {
    eventLog_ = std::move(entries);
    return {EventLogChanged{}};
}

void Guild::inviteAccepted() {
    invitePending_ = false;
    joinedName_ = inviteGuild_;
}

void Guild::inviteDeclined() {
    invitePending_ = false;
}

void Guild::selectRank(int index) {
    selectedRank_ = index;
    draft_ = RankDraft{};
    if (index < 1 || index > static_cast<int>(roster_.ranks.size())) return;
    const Rank& r = roster_.ranks[static_cast<size_t>(index) - 1];
    draft_.rights = r.rights;
    draft_.goldLimit = r.goldLimit;
    draft_.tabRights = r.bankTabRights;
    draft_.tabSlots = r.bankTabSlotsPerDay;
}

bool Guild::isMember(uint32_t ownGuildId) const {
    // Three records, and the newest wins: a guild joined this session is in
    // none of the others until the next login.
    return confirmed_ || !name_.empty() || ownGuildId != 0;
}

uint32_t Guild::rankIndexOf(uint64_t playerGuid) const {
    if (!hasRoster_) return kNoRank;
    for (const auto& m : roster_.members)
        if (m.guid == playerGuid) return m.rankIndex;
    return kNoRank;
}

uint32_t Guild::rightsOf(uint64_t playerGuid) const {
    const uint32_t idx = rankIndexOf(playerGuid);
    if (idx >= roster_.ranks.size()) return 0;
    return roster_.ranks[idx].rights;
}

} // namespace wowee::guild
