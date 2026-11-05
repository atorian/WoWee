#pragma once

#include "arena/domain/records.hpp"
#include "arena/services/ports.hpp"
#include "foundation/event_bus.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class ChatLog;
class FrameXmlEvents;

/// The arena service's port, implemented over the world connection.
class ArenaGateway final : public arena::IArenaGateway {
public:
    explicit ArenaGateway(PacketSender& sender) : sender_(sender) {}

    [[nodiscard]] bool online() const override;
    [[nodiscard]] bool connected() const override;
    void sendAccept() override;
    void sendDecline() override;
    void sendInvite(uint32_t teamId, const std::string& name) override;
    void sendLeave(uint32_t teamId) override;
    void sendRemove(uint32_t teamId, const std::string& name) override;
    void sendLeader(uint32_t teamId, const std::string& name) override;
    void sendDisband(uint32_t teamId) override;
    void sendRosterRequest(uint32_t teamId) override;

private:
    PacketSender& sender_;
};

/// SMSG_ARENA_TEAM_ROSTER; nothing when the header is short.
[[nodiscard]] std::optional<arena::ArenaTeamRoster> readArenaTeamRoster(network::Packet& packet);
/// The server's numbers, as AzerothCore writes them.
[[nodiscard]] arena::ArenaCommand arenaCommand(uint32_t command);
[[nodiscard]] arena::ArenaCommandError arenaCommandError(uint32_t error);
[[nodiscard]] arena::ArenaTeamChange arenaTeamChange(uint32_t event);
[[nodiscard]] arena::ArenaMatchError arenaMatchError(uint32_t error);

/// What the player sees of the arena teams: ARENA_TEAM_UPDATE and
/// ARENA_TEAM_ROSTER_UPDATE, and the server's answers in the chat.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeArenaNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
