#pragma once

#include "core/event_bus.hpp"
#include "mail/services/ports.hpp"

#include <vector>

namespace wowee::game {

class PacketSender;
class InventoryHandler;
class InventoryQueries;
class QueryCacheHandler;
class ChatLog;
class FrameXmlEvents;

/// The mail service's ports, implemented over the world connection, the bags,
/// the refund windows and the name and item caches.
class MailGateway final : public mail::IMailGateway, public mail::IMailWorld {
public:
    MailGateway(PacketSender& sender, InventoryHandler& inventory, InventoryQueries& world,
                const QueryCacheHandler& names)
        : sender_(sender), inventory_(inventory), world_(world), names_(names) {}

    // IMailGateway
    [[nodiscard]] bool inWorld() const override;
    [[nodiscard]] bool connected() const override;
    [[nodiscard]] int maxEnclosures() const override;
    void sendListRequest(uint64_t mailbox) override;
    void sendLetter(uint64_t mailbox, const std::string& recipient, const std::string& subject,
                    const std::string& body, uint64_t money, uint64_t cod,
                    const std::vector<uint64_t>& itemGuids) override;
    void sendTakeMoney(uint64_t mailbox, uint32_t mailId) override;
    void sendTakeItem(uint64_t mailbox, uint32_t mailId, uint32_t itemGuidLow) override;
    void sendReturn(uint64_t mailbox, uint32_t mailId) override;
    void sendDelete(uint64_t mailbox, uint32_t mailId) override;
    void sendMarkRead(uint64_t mailbox, uint32_t mailId) override;

    // IMailWorld
    [[nodiscard]] std::optional<mail::Enclosure> backpackItem(int index) const override;
    [[nodiscard]] std::optional<mail::Enclosure> bagItem(int bag, int slot) const override;
    Refund refundWindow(uint64_t itemGuid) override;
    void chooseDefaultStationery() override;
    [[nodiscard]] std::string playerName(uint64_t guid) const override;
    [[nodiscard]] std::string creatureName(uint32_t entry) const override;
    [[nodiscard]] std::string gameObjectName(uint32_t entry) const override;
    [[nodiscard]] std::string itemName(uint32_t itemEntry) const override;
    void requestItem(uint32_t itemEntry) override;
    void requestPlayerName(uint64_t guid) override;

private:
    PacketSender& sender_;
    InventoryHandler& inventory_;
    InventoryQueries& world_;
    const QueryCacheHandler& names_;
};

/// What the player sees and hears of the mailbox: the MAIL_* events, the chat
/// lines, and the bags and coin redrawn after something was taken.
[[nodiscard]] std::vector<core::EventBus::Subscription>
subscribeMailNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                     InventoryHandler& inventory);

} // namespace wowee::game
