#include "guild/services/guild_service.hpp"

#include "foundation/event_bus.hpp"

namespace wowee::guild {

namespace {

/// The guild of a client with no game: never a member, and every request
/// dropped.
class NoGuild final : public IGuild {
public:
    bool isInGuild() const override { return false; }
    const std::string& getGuildName() const override { return empty_; }
    const std::vector<std::string>& getGuildRankNames() const override { return rankNames_; }
    const Roster& getGuildRoster() const override { return roster_; }
    bool hasGuildRoster() const override { return false; }
    const Summary& getGuildInfoData() const override { return summary_; }
    bool hasGuildInfoData() const override { return false; }
    const Identity& getGuildQueryData() const override { return identity_; }
    bool hasPendingGuildInvite() const override { return false; }
    const std::string& getPendingGuildInviterName() const override { return empty_; }
    const std::string& getPendingGuildInviteGuildName() const override { return empty_; }
    uint32_t getPlayerGuildRankIndex() const override { return Guild::kNoRank; }
    uint32_t getPlayerGuildRankRights() const override { return 0; }
    const std::vector<EventLogEntry>& getGuildEventLog() const override { return eventLog_; }
    const std::string& lookupGuildName(uint32_t) override { return empty_; }
    void requestGuildInfo() override {}
    void requestGuildRoster() override {}
    void requestGuildEventLog() override {}
    void setGuildInfoText(const std::string&) override {}
    void setGuildMotd(const std::string&) override {}
    void promoteGuildMember(const std::string&) override {}
    void demoteGuildMember(const std::string&) override {}
    void leaveGuild() override {}
    void inviteToGuild(const std::string&) override {}
    void kickGuildMember(const std::string&) override {}
    void disbandGuild() override {}
    void setGuildLeader(const std::string&) override {}
    void setGuildPublicNote(const std::string&, const std::string&) override {}
    void setGuildOfficerNote(const std::string&, const std::string&) override {}
    void acceptGuildInvite() override {}
    void declineGuildInvite() override {}
    void createGuild(const std::string&) override {}
    void addGuildRank(const std::string&) override {}
    void delGuildRank() override {}
    void setSelectedGuildRank(int) override {}
    int getSelectedGuildRank() const override { return 0; }
    RankDraft& pendingGuildRankRef() override { return draft_; }
    void saveGuildRank(const std::string&) override {}

private:
    std::string empty_;
    std::vector<std::string> rankNames_;
    Roster roster_;
    Summary summary_;
    Identity identity_;
    std::vector<EventLogEntry> eventLog_;
    RankDraft draft_;
};

NoGuild kNoGuild;

} // namespace

IGuild& orNoGuild(IGuild* guild) {
    return guild ? *guild : kNoGuild;
}

GuildService::GuildService(IGuildGateway& gateway, IGuildMember& member,
                           IGuildNameStore& store, core::EventBus& events)
    : gateway_(gateway), member_(member), store_(store), events_(events) {
    names_.restore(store_.load());
}

bool GuildService::isInGuild() const {
    return guild_.isMember(member_.ownGuildId());
}

uint32_t GuildService::getPlayerGuildRankIndex() const {
    return guild_.rankIndexOf(member_.playerGuid());
}

uint32_t GuildService::getPlayerGuildRankRights() const {
    return guild_.rightsOf(member_.playerGuid());
}

const std::string& GuildService::lookupGuildName(uint32_t guildId) {
    const std::string& name = names_.find(guildId);
    if (name.empty() && names_.shouldQuery(guildId, GuildNames::Clock::now())) query(guildId);
    return name;
}

void GuildService::query(uint32_t guildId) {
    if (gateway_.canQuery()) gateway_.queryGuild(guildId);
}

// ---- Requests ----

void GuildService::requestGuildInfo() {
    if (gateway_.canSend()) gateway_.requestInfo();
}

void GuildService::requestGuildRoster() {
    if (gateway_.canSend()) gateway_.requestRoster();
}

void GuildService::requestGuildEventLog() {
    if (gateway_.canSend()) gateway_.requestEventLog();
}

void GuildService::setGuildInfoText(const std::string& text) {
    if (!gateway_.canSend()) return;
    gateway_.sendInfoText(text);
    gateway_.requestRoster();
}

void GuildService::setGuildMotd(const std::string& motd) {
    if (!gateway_.canSend()) return;
    gateway_.sendMotd(motd);
    events_.publish(MotdSent{});
}

void GuildService::promoteGuildMember(const std::string& name) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(PlayerNameRequired{}); return; }
    gateway_.sendPromote(name);
    events_.publish(PromotionRequested{name});
}

void GuildService::demoteGuildMember(const std::string& name) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(PlayerNameRequired{}); return; }
    gateway_.sendDemote(name);
    events_.publish(DemotionRequested{name});
}

void GuildService::leaveGuild() {
    if (!gateway_.canSend()) return;
    gateway_.sendLeave();
    events_.publish(LeaveRequested{});
}

void GuildService::inviteToGuild(const std::string& name) {
    if (!gateway_.canSend()) return;
    if (name.empty()) { events_.publish(PlayerNameRequired{}); return; }
    gateway_.sendInvite(name);
    events_.publish(InviteRequested{name});
}

void GuildService::kickGuildMember(const std::string& name) {
    if (gateway_.canSend()) gateway_.sendRemove(name);
}

void GuildService::disbandGuild() {
    if (gateway_.canSend()) gateway_.sendDisband();
}

void GuildService::setGuildLeader(const std::string& name) {
    if (gateway_.canSend()) gateway_.sendLeader(name);
}

void GuildService::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (gateway_.canSend()) gateway_.sendPublicNote(name, note);
}

void GuildService::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (gateway_.canSend()) gateway_.sendOfficerNote(name, note);
}

void GuildService::acceptGuildInvite() {
    if (!gateway_.canSend()) return;
    guild_.inviteAccepted();
    gateway_.sendAcceptInvite();
    // The server sends the join to the guild, not a roster to the joiner, so
    // nothing fills the guild tab unless the client asks.
    gateway_.requestRoster();
}

void GuildService::declineGuildInvite() {
    if (!gateway_.canSend()) return;
    guild_.inviteDeclined();
    gateway_.sendDeclineInvite();
}

void GuildService::createGuild(const std::string& guildName) {
    if (gateway_.canSend()) gateway_.sendCreate(guildName);
}

void GuildService::addGuildRank(const std::string& rankName) {
    if (!gateway_.canSend()) return;
    gateway_.sendAddRank(rankName);
    gateway_.requestRoster();
}

void GuildService::delGuildRank() {
    if (!gateway_.canSend()) return;
    gateway_.sendDeleteRank();
    gateway_.requestRoster();
}

void GuildService::saveGuildRank(const std::string& rankName) {
    const int selected = guild_.selectedRank();
    if (selected < 1 || !gateway_.canSend()) return;
    // Ranks are zero-based on the wire and one-based in the panel.
    gateway_.sendRank(static_cast<uint32_t>(selected - 1), rankName, guild_.rankDraft());
    gateway_.requestRoster();
}

// ---- What the server sent ----

void GuildService::onSummary(Summary summary) {
    apply(guild_.summaryReceived(std::move(summary), member_.ownGuildId()));
}

void GuildService::onRoster(Roster roster) {
    auto changes = guild_.rosterReceived(std::move(roster));
    // The name comes from the guild's own query, which needs the id - and the
    // id is on the player once the server has moved them into the guild.
    if (guild_.name().empty()) {
        if (const uint32_t id = member_.ownGuildId()) query(id);
    }
    apply(changes);
}

void GuildService::onIdentity(Identity identity) {
    names_.answered(identity.guildId);
    apply(guild_.identityReceived(std::move(identity), member_.ownGuildId()));
}

void GuildService::onEvent(const ServerEvent& event) {
    apply(guild_.eventReceived(event));
}

void GuildService::onInvite(std::string inviter, std::string guildName) {
    apply(guild_.inviteReceived(std::move(inviter), std::move(guildName)));
}

void GuildService::onInviteDeclined(std::string name) {
    events_.publish(InviteDeclined{std::move(name)});
}

void GuildService::onCommandResult(uint32_t command, uint32_t errorCode, const std::string& name) {
    apply(guild_.commandResult(command, errorCode, name));
}

void GuildService::onEventLog(std::vector<EventLogEntry> entries) {
    apply(guild_.eventLogReceived(std::move(entries)));
}

void GuildService::apply(const std::vector<GuildChange>& changes) {
    bool rosterOutdated = false;
    bool tabardChanged = false;
    for (const auto& change : changes) {
        if (const auto* learned = std::get_if<GuildNameLearned>(&change)) {
            if (names_.learn(learned->guildId, learned->name)) store_.save(names_.all());
        }
        rosterOutdated |= std::holds_alternative<RosterOutdated>(change);
        tabardChanged |= std::holds_alternative<TabardChanged>(change);
        std::visit([this](const auto& e) { events_.publish(e); }, change);
    }
    // A new tabard is in the guild's query, not the roster.
    if (tabardChanged) {
        if (const uint32_t id = member_.ownGuildId()) query(id);
    }
    if (rosterOutdated) requestGuildRoster();
}

} // namespace wowee::guild
