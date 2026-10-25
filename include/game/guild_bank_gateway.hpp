#pragma once

#include "core/event_bus.hpp"
#include "guild_bank/domain/records.hpp"
#include "guild_bank/services/ports.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game {

class PacketSender;
class InventoryHandler;
class InventoryQueries;
class ChatLog;
class FrameXmlEvents;

/// The guild bank service's ports, implemented over the world connection, the
/// guild and the item cache.
class GuildBankGateway final : public guild_bank::IGuildBankGateway, public guild_bank::IGuildBankWorld {
public:
    GuildBankGateway(PacketSender& sender, InventoryHandler& inventory, InventoryQueries& world)
        : sender_(sender), inventory_(inventory), world_(world) {}

    // IGuildBankGateway
    [[nodiscard]] bool online() const override;
    void sendActivate(uint64_t banker) override;
    void sendQueryTab(uint64_t banker, uint8_t tab) override;
    void sendBuyTab(uint64_t banker, uint8_t tab) override;
    void sendDepositMoney(uint64_t banker, uint32_t amount) override;
    void sendWithdrawMoney(uint64_t banker, uint32_t amount) override;
    void sendWithdrawItem(uint64_t banker, uint8_t tab, uint8_t bankSlot,
                          uint8_t destBag, uint8_t destSlot, uint32_t splitCount) override;
    void sendDepositItem(uint64_t banker, uint8_t tab, uint8_t bankSlot,
                         uint8_t srcBag, uint8_t srcSlot) override;
    void sendQueryText(uint8_t tab) override;
    void sendTabText(uint8_t tab, const std::string& text) override;
    void sendUpdateTab(uint64_t banker, uint8_t tab, const std::string& name, const std::string& icon) override;
    void sendLogQuery(uint8_t tab) override;

    // IGuildBankWorld
    [[nodiscard]] bool inGuild() const override;
    void requestItem(uint32_t itemEntry) override;

private:
    PacketSender& sender_;
    InventoryHandler& inventory_;
    InventoryQueries& world_;
};

/// One tab's log as MSG_GUILD_BANK_LOG_QUERY's reply carries it.
struct GuildBankLogReply {
    uint8_t tab = 0;
    std::vector<guild_bank::GuildBankLogEntry> entries;
};

/// The log reply; nothing when it names no tab the vault has, or when an
/// entry is cut short inside the fields its type calls for.
[[nodiscard]] std::optional<GuildBankLogReply> readGuildBankLog(network::Packet& packet);

/// What the player sees of the vault: the GUILDBANK* events, and the two
/// refusals the player is told about.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeGuildBankNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml);

} // namespace wowee::game
