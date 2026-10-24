#pragma once

#include "battleground/domain/records.hpp"
#include "battleground/services/ports.hpp"
#include "core/event_bus.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class SocialQueries;
class ChatLog;
class FrameXmlEvents;
class EntityManager;

/// The battleground service's ports, implemented over the world connection,
/// the client's name cache and the map the player is on.
class BattlegroundGateway final : public battleground::IBattlegroundGateway,
                                  public battleground::IBattlegroundWorld {
public:
    BattlegroundGateway(PacketSender& sender, SocialQueries& world) : sender_(sender), world_(world) {}

    // IBattlegroundGateway
    [[nodiscard]] bool online() const override;
    void sendPort(uint8_t arenaType, uint32_t bgTypeId, bool accept) override;
    void sendJoin(uint64_t battlemaster, uint32_t bgTypeId, uint32_t instanceId, bool asGroup) override;
    void sendListRequest(uint32_t bgTypeId) override;
    void sendReportAfk(uint64_t playerGuid) override;
    void sendLeave() override;
    void sendScoreRequest() override;
    void sendPositionsRequest() override;

    // IBattlegroundWorld
    [[nodiscard]] std::string playerName(uint64_t guid) const override;
    [[nodiscard]] bool inBattleground() const override;

private:
    PacketSender& sender_;
    SocialQueries& world_;
};

/// SMSG_BATTLEFIELD_LIST; nothing when it is cut short before the instances.
[[nodiscard]] std::optional<battleground::AvailableBgInfo> readBattlefieldList(network::Packet& packet);
/// MSG_BATTLEGROUND_PLAYER_POSITIONS: both teams' lists, in that order.
[[nodiscard]] std::vector<battleground::BgPlayerPosition> readBattlegroundPositions(network::Packet& packet);
/// MSG_PVP_LOG_DATA, each row named from the unit in view; nothing when it is
/// cut short before the player list.
[[nodiscard]] std::optional<battleground::BgScoreboardData> readPvpLog(network::Packet& packet,
                                                                       const EntityManager& entities);
/// SMSG_GROUP_JOINED_BATTLEGROUND's refusals, as AzerothCore writes them.
[[nodiscard]] battleground::JoinRefusal joinRefusal(int32_t result);

/// What the player sees of the battlegrounds: UPDATE_BATTLEFIELD_STATUS and
/// UPDATE_BATTLEFIELD_SCORE, the queue lines and the server's answers in the
/// chat, and the refusals on the error line as well.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeBattlegroundNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
