#pragma once

#include "foundation/event_bus.hpp"
#include "guild/domain/records.hpp"
#include "guild/services/ports.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::core { class CVarStore; }
namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class SocialQueries;
class ChatLog;
class FrameXmlEvents;
struct GuildEventData;

/// The guild service's ports, implemented over the world connection and the
/// player's entity.
class GuildGateway final : public guild::IGuildGateway, public guild::IGuildMember {
public:
    GuildGateway(PacketSender& sender, SocialQueries& world) : sender_(sender), world_(world) {}

    // IGuildGateway
    [[nodiscard]] bool canSend() const override;
    [[nodiscard]] bool canQuery() const override;
    void requestInfo() override;
    void requestRoster() override;
    void requestEventLog() override;
    void queryGuild(uint32_t guildId) override;
    void sendInfoText(const std::string& text) override;
    void sendMotd(const std::string& motd) override;
    void sendPromote(const std::string& name) override;
    void sendDemote(const std::string& name) override;
    void sendLeave() override;
    void sendInvite(const std::string& name) override;
    void sendRemove(const std::string& name) override;
    void sendDisband() override;
    void sendLeader(const std::string& name) override;
    void sendPublicNote(const std::string& name, const std::string& note) override;
    void sendOfficerNote(const std::string& name, const std::string& note) override;
    void sendAcceptInvite() override;
    void sendDeclineInvite() override;
    void sendCreate(const std::string& guildName) override;
    void sendAddRank(const std::string& rankName) override;
    void sendDeleteRank() override;
    void sendRank(uint32_t rankId, const std::string& rankName, const guild::RankDraft& rank) override;

    // IGuildMember
    [[nodiscard]] uint64_t playerGuid() const override;
    [[nodiscard]] uint32_t ownGuildId() const override;

private:
    PacketSender& sender_;
    SocialQueries& world_;
};

/// Guild names kept in guild_names.tsv in the user's wowee directory, one
/// "id<TAB>name" per line.
class GuildNameFile final : public guild::IGuildNameStore {
public:
    [[nodiscard]] std::unordered_map<uint32_t, std::string> load() const override;
    void save(const std::unordered_map<uint32_t, std::string>& names) override;
};

/// The guild id a player unit carries (PLAYER_GUILDID), or 0 for anything else.
[[nodiscard]] uint32_t playerGuildId(const SocialQueries& world, uint64_t guid);

/// MSG_GUILD_EVENT_LOG_QUERY's reply; nothing when it is too short to hold
/// even a count.
[[nodiscard]] std::optional<std::vector<guild::EventLogEntry>> readGuildEventLog(network::Packet& packet);
[[nodiscard]] guild::ServerEvent toServerEvent(const GuildEventData& data);

/// What the player sees of the guild: the chat lines, the addon events, the
/// refusals on the error line, and the bank's balance.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeGuildNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                      SocialQueries& world, const core::CVarStore& cvars);

} // namespace wowee::game
