#include "game/guild_bank_gateway.hpp"

#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/inventory_handler.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "guild_bank/domain/events.hpp"
#include "network/packet.hpp"

namespace wowee::game {

bool GuildBankGateway::online() const {
    return sender_.characterInWorld();
}

// CMSG_GUILD_BANKER_ACTIVATE registers this session with the guild banker so
// the server sends the tab list and contents (SMSG_GUILD_BANK_LIST). Without
// it the follow-up tab query is rejected and no bank list ever arrives.
void GuildBankGateway::sendActivate(uint64_t banker) {
    sender_.send(GuildBankerActivatePacket::build(banker));
}

void GuildBankGateway::sendQueryTab(uint64_t banker, uint8_t tab) {
    // Asking for the full tab, not a slot refresh. The byte reaches
    // SendBankList as "with content" on the cores that read it, so a false here
    // asked the server for a tab and told it not to send what is in it. This
    // client keeps no cached tab to refresh, so there is nothing a partial
    // update could be for.
    sender_.send(GuildBankQueryTabPacket::build(banker, tab, true));
}

void GuildBankGateway::sendBuyTab(uint64_t banker, uint8_t tab) {
    sender_.send(GuildBankBuyTabPacket::build(banker, tab));
}

void GuildBankGateway::sendDepositMoney(uint64_t banker, uint32_t amount) {
    sender_.send(GuildBankDepositMoneyPacket::build(banker, amount));
}

void GuildBankGateway::sendWithdrawMoney(uint64_t banker, uint32_t amount) {
    sender_.send(GuildBankWithdrawMoneyPacket::build(banker, amount));
}

void GuildBankGateway::sendWithdrawItem(uint64_t banker, uint8_t tab, uint8_t bankSlot,
                                        uint8_t destBag, uint8_t destSlot, uint32_t splitCount) {
    sender_.send(GuildBankSwapItemsPacket::buildBankToInventory(banker, tab, bankSlot, destBag, destSlot,
                                                                splitCount));
}

// CMSG_GUILD_BANK_SWAP_ITEMS has no server-side auto-store for this direction
// (that path forces bank to character), so the slot is chosen before it gets
// here.
void GuildBankGateway::sendDepositItem(uint64_t banker, uint8_t tab, uint8_t bankSlot,
                                       uint8_t srcBag, uint8_t srcSlot) {
    sender_.send(GuildBankSwapItemsPacket::buildInventoryToBank(banker, tab, bankSlot, srcBag, srcSlot));
}

void GuildBankGateway::sendQueryText(uint8_t tab) {
    network::Packet p(wireOpcode(Opcode::MSG_QUERY_GUILD_BANK_TEXT));
    p.writeUInt8(tab);
    sender_.send(p);
}

void GuildBankGateway::sendTabText(uint8_t tab, const std::string& text) {
    sender_.send(GuildBankSetTabTextPacket::build(tab, text));
}

// CMSG_GUILD_BANK_UPDATE_TAB: the banker's guid, the tab, its name and icon.
void GuildBankGateway::sendUpdateTab(uint64_t banker, uint8_t tab, const std::string& name,
                                     const std::string& icon) {
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_UPDATE_TAB));
    p.writeUInt64(banker);
    p.writeUInt8(tab);
    p.writeString(name);
    p.writeString(icon);
    sender_.send(p);
}

void GuildBankGateway::sendLogQuery(uint8_t tab) {
    network::Packet p(wireOpcode(Opcode::MSG_GUILD_BANK_LOG_QUERY));
    p.writeUInt8(tab);
    sender_.send(p);
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY: tab ", static_cast<int>(tab));
}

bool GuildBankGateway::inGuild() const {
    return world_.inGuild();
}

void GuildBankGateway::requestItem(uint32_t itemEntry) {
    inventory_.ensureItemInfo(itemEntry);
}

// The reply, which shares its opcode with the request.
//
//   uint8 tab
//   uint8 count
//   per entry:
//     int8   type
//     uint64 playerGuid
//     the payload the type calls for - an item and a count, those plus a
//     destination tab for a move, or a sum of money for everything else
//     uint32 secondsAgo
std::optional<GuildBankLogReply> readGuildBankLog(network::Packet& packet) {
    if (!packet.hasRemaining(2)) { packet.skipAll(); return std::nullopt; }
    GuildBankLogReply reply;
    reply.tab = packet.readUInt8();
    const uint8_t count = packet.readUInt8();
    if (reply.tab > guild_bank::kMoneyLogTab) {
        LOG_WARNING("MSG_GUILD_BANK_LOG_QUERY reply: tab ", static_cast<int>(reply.tab),
                    " is not one the vault has; the log is not shown");
        packet.skipAll();
        return std::nullopt;
    }
    auto cutShort = [&](const guild_bank::GuildBankLogEntry& e) {
        LOG_WARNING("MSG_GUILD_BANK_LOG_QUERY reply: tab ", static_cast<int>(reply.tab), " entry ",
                    reply.entries.size(), " of type ", static_cast<int>(e.type),
                    " is cut short; the log shown stays as it was");
        packet.skipAll();
        return std::nullopt;
    };
    reply.entries.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(9)) break;
        guild_bank::GuildBankLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        switch (e.type) {
            case 1: case 2:                 // deposit / withdraw an item
                if (!packet.hasRemaining(8)) return cutShort(e);
                e.itemId = packet.readUInt32();
                e.count  = packet.readUInt32();
                break;
            case 3: case 7:                 // moved between tabs
                if (!packet.hasRemaining(9)) return cutShort(e);
                e.itemId   = packet.readUInt32();
                e.count    = packet.readUInt32();
                e.otherTab = packet.readUInt8();
                break;
            default:                        // money, in copper
                if (!packet.hasRemaining(4)) return cutShort(e);
                e.count = packet.readUInt32();
                break;
        }
        if (!packet.hasRemaining(4)) break;
        e.secondsAgo = packet.readUInt32();
        reply.entries.push_back(e);
    }
    packet.skipAll();
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY reply: tab ", static_cast<int>(reply.tab),
             ", ", reply.entries.size(), " entries");
    return reply;
}

std::vector<core::EventBus::Subscription>
subscribeGuildBankNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml) {
    using namespace guild_bank;
    std::vector<core::EventBus::Subscription> s;
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    s.push_back(events.subscribe<VaultOpened>([addon](const auto&) { addon("GUILDBANKFRAME_OPENED"); }));
    s.push_back(events.subscribe<VaultClosed>([addon](const auto&) { addon("GUILDBANKFRAME_CLOSED"); }));
    // The one that runs GuildBankFrame_SelectAvailableTab, which picks a tab
    // the player may look in and draws it.
    s.push_back(events.subscribe<TabsChanged>([addon](const auto&) { addon("GUILDBANK_UPDATE_TABS"); }));
    s.push_back(events.subscribe<SlotsChanged>([addon](const auto&) { addon("GUILDBANKBAGSLOTS_CHANGED"); }));
    // The money line and the withdraw allowance are read from GetGuildBankMoney
    // and GetGuildBankWithdrawMoney, and only again when these say so.
    s.push_back(events.subscribe<MoneyChanged>([addon](const auto&) { addon("GUILDBANK_UPDATE_MONEY"); }));
    s.push_back(events.subscribe<WithdrawAllowanceChanged>([addon](const auto&) {
        addon("GUILDBANK_UPDATE_WITHDRAWMONEY");
    }));
    // The info box reads the tab back with GetGuildBankText, counting from one.
    s.push_back(events.subscribe<TabTextChanged>([addon](const TabTextChanged& e) {
        addon("GUILDBANK_UPDATE_TEXT", {std::to_string(e.tab)});
    }));
    s.push_back(events.subscribe<LogChanged>([addon](const auto&) { addon("GUILDBANKLOG_UPDATE"); }));
    s.push_back(events.subscribe<OpenRefused>([&events, &frameXml](const auto&) {
        raiseUiError(events, frameXml, "You are not in a guild.");
    }));
    s.push_back(events.subscribe<TabFull>([&chat](const auto&) {
        chat.addSystemChatMessage("This guild bank tab is full.");
    }));
    return s;
}

} // namespace wowee::game
