#pragma once

#include "guild/domain/records.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee::guild {

/// Requests to the server about guilds. Implemented by the protocol layer.
class IGuildGateway {
public:
    virtual ~IGuildGateway() = default;

    /// In the world with a connection; requests are dropped otherwise.
    [[nodiscard]] virtual bool canSend() const = 0;
    /// Connected at all. A guild's name may be asked for at the character
    /// screen too: the server accepts CMSG_GUILD_QUERY before login.
    [[nodiscard]] virtual bool canQuery() const = 0;

    virtual void requestInfo() = 0;
    virtual void requestRoster() = 0;
    virtual void requestEventLog() = 0;
    virtual void queryGuild(uint32_t guildId) = 0;
    virtual void sendInfoText(const std::string& text) = 0;
    virtual void sendMotd(const std::string& motd) = 0;
    virtual void sendPromote(const std::string& name) = 0;
    virtual void sendDemote(const std::string& name) = 0;
    virtual void sendLeave() = 0;
    virtual void sendInvite(const std::string& name) = 0;
    virtual void sendRemove(const std::string& name) = 0;
    virtual void sendDisband() = 0;
    virtual void sendLeader(const std::string& name) = 0;
    virtual void sendPublicNote(const std::string& name, const std::string& note) = 0;
    virtual void sendOfficerNote(const std::string& name, const std::string& note) = 0;
    virtual void sendAcceptInvite() = 0;
    virtual void sendDeclineInvite() = 0;
    virtual void sendCreate(const std::string& guildName) = 0;
    virtual void sendAddRank(const std::string& rankName) = 0;
    virtual void sendDeleteRank() = 0;
    /// The rank's zero-based id and everything the rank carries.
    virtual void sendRank(uint32_t rankId, const std::string& rankName, const RankDraft& rank) = 0;
};

/// The player whose guild this is.
class IGuildMember {
public:
    virtual ~IGuildMember() = default;

    [[nodiscard]] virtual uint64_t playerGuid() const = 0;
    /// The player's own guild id: their live field first, which the server
    /// keeps current, then the character list's copy from login.
    [[nodiscard]] virtual uint32_t ownGuildId() const = 0;
};

/// Where guild names are kept between sessions.
class IGuildNameStore {
public:
    virtual ~IGuildNameStore() = default;

    [[nodiscard]] virtual std::unordered_map<uint32_t, std::string> load() const = 0;
    virtual void save(const std::unordered_map<uint32_t, std::string>& names) = 0;
};

} // namespace wowee::guild
