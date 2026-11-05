#pragma once

#include "foundation/event_bus.hpp"
#include "lfg/domain/records.hpp"
#include "lfg/services/ports.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::game {

class PacketSender;
class SocialQueries;
class ChatLog;
class FrameXmlEvents;

/// The dungeon finder's ports, implemented over the world connection, the
/// dungeon table and the name caches. It sends nothing on a realm with no
/// dungeon finder.
class LfgGateway final : public lfg::ILfgGateway, public lfg::ILfgWorld {
public:
    LfgGateway(PacketSender& sender, SocialQueries& world) : sender_(sender), world_(world) {}

    // ILfgGateway
    [[nodiscard]] bool online() const override;
    [[nodiscard]] bool connected() const override;
    void sendJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) override;
    void sendLeave() override;
    void sendSetRoles(uint8_t roles) override;
    void sendProposalAnswer(uint32_t proposalId, bool accept) override;
    void sendTeleport(bool toLfgDungeon) override;
    void sendBootVote(bool vote) override;
    void sendPlayerLockInfoRequest() override;
    void sendPartyLockInfoRequest() override;

    // ILfgWorld
    [[nodiscard]] std::string dungeonName(uint32_t dungeonId) const override;
    [[nodiscard]] std::string playerName(uint64_t guid) const override;
    void requestPlayerName(uint64_t guid) override;
    [[nodiscard]] std::optional<std::string> unitName(uint64_t guid) const override;

private:
    PacketSender& sender_;
    SocialQueries& world_;
};

/// The server's numbers for a refusal, as AzerothCore writes them.
[[nodiscard]] lfg::JoinRefusal lfgJoinRefusal(uint32_t result);
[[nodiscard]] lfg::TeleportRefusal lfgTeleportRefusal(uint32_t reason);

/// What the player sees of the dungeon finder: the LFG_* events, the chat
/// lines and the refusals on the error line. `names` gives the dungeons their
/// names.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeLfgNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml, SocialQueries& names);

} // namespace wowee::game
