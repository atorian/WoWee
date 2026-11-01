#pragma once

#include "guild/domain/guild.hpp"
#include "guild/domain/guild_names.hpp"
#include "guild/services/guild.hpp"
#include "guild/services/ports.hpp"

#include <string>
#include <vector>

namespace wowee::core { class EventBus; }

namespace wowee::guild {

class GuildService final : public IGuild, public IGuildServerFeed {
public:
    GuildService(IGuildGateway& gateway, IGuildMember& member, IGuildNameStore& store,
                 core::EventBus& events);

    // IGuild: reading
    [[nodiscard]] bool isInGuild() const override;
    [[nodiscard]] const std::string& getGuildName() const override { return guild_.name(); }
    [[nodiscard]] const std::vector<std::string>& getGuildRankNames() const override { return guild_.rankNames(); }
    [[nodiscard]] const Roster& getGuildRoster() const override { return guild_.roster(); }
    [[nodiscard]] bool hasGuildRoster() const override { return guild_.hasRoster(); }
    [[nodiscard]] const Summary& getGuildInfoData() const override { return guild_.summary(); }
    [[nodiscard]] bool hasGuildInfoData() const override { return guild_.summary().isValid(); }
    [[nodiscard]] const Identity& getGuildQueryData() const override { return guild_.identity(); }
    [[nodiscard]] bool hasPendingGuildInvite() const override { return guild_.hasPendingInvite(); }
    [[nodiscard]] const std::string& getPendingGuildInviterName() const override { return guild_.inviterName(); }
    [[nodiscard]] const std::string& getPendingGuildInviteGuildName() const override { return guild_.inviteGuildName(); }
    [[nodiscard]] uint32_t getPlayerGuildRankIndex() const override;
    [[nodiscard]] uint32_t getPlayerGuildRankRights() const override;
    [[nodiscard]] const std::vector<EventLogEntry>& getGuildEventLog() const override { return guild_.eventLog(); }
    const std::string& lookupGuildName(uint32_t guildId) override;

    // IGuild: requests
    void requestGuildInfo() override;
    void requestGuildRoster() override;
    void requestGuildEventLog() override;
    void setGuildInfoText(const std::string& text) override;
    void setGuildMotd(const std::string& motd) override;
    void promoteGuildMember(const std::string& name) override;
    void demoteGuildMember(const std::string& name) override;
    void leaveGuild() override;
    void inviteToGuild(const std::string& name) override;
    void kickGuildMember(const std::string& name) override;
    void disbandGuild() override;
    void setGuildLeader(const std::string& name) override;
    void setGuildPublicNote(const std::string& name, const std::string& note) override;
    void setGuildOfficerNote(const std::string& name, const std::string& note) override;
    void acceptGuildInvite() override;
    void declineGuildInvite() override;
    void createGuild(const std::string& guildName) override;
    void addGuildRank(const std::string& rankName) override;
    void delGuildRank() override;
    void setSelectedGuildRank(int index) override { guild_.selectRank(index); }
    [[nodiscard]] int getSelectedGuildRank() const override { return guild_.selectedRank(); }
    RankDraft& pendingGuildRankRef() override { return guild_.rankDraft(); }
    void saveGuildRank(const std::string& rankName) override;

    // IGuildServerFeed
    void onSummary(Summary summary) override;
    void onRoster(Roster roster) override;
    void onIdentity(Identity identity) override;
    void onEvent(const ServerEvent& event) override;
    void onInvite(std::string inviter, std::string guildName) override;
    void onInviteDeclined(std::string name) override;
    void onCommandResult(uint32_t command, uint32_t errorCode, const std::string& name) override;
    void onEventLog(std::vector<EventLogEntry> entries) override;
    void beginCharacter() override { guild_.beginCharacter(); }

private:
    /// Publishes each change, then does what some of them ask of the client.
    void apply(const std::vector<GuildChange>& changes);
    void query(uint32_t guildId);

    IGuildGateway& gateway_;
    IGuildMember& member_;
    IGuildNameStore& store_;
    core::EventBus& events_;
    Guild guild_;
    GuildNames names_;
};

} // namespace wowee::guild
