#include "game/loot_gateway.hpp"

#include "core/cvar_store.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_utils.hpp"
#include "game/inventory_handler.hpp"
#include "game/item_text.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/world_packets.hpp"
#include "loot/domain/events.hpp"
#include "network/packet.hpp"

namespace wowee::game {

bool LootGateway::online() const {
    return sender_.characterInWorld();
}

bool LootGateway::mayLoot() {
    return sender_.worldCommandAllowed("loot");
}

void LootGateway::sendLoot(uint64_t source) {
    LOG_INFO("Looting target 0x", std::hex, source, std::dec);
    sender_.send(LootPacket::build(source));
}

void LootGateway::sendAutostore(uint8_t slot) {
    sender_.send(AutostoreLootItemPacket::build(slot));
}

void LootGateway::sendLootMoney() {
    sender_.send(LootMoneyPacket::build());
}

void LootGateway::sendRelease(uint64_t source) {
    sender_.send(LootReleasePacket::build(source));
}

void LootGateway::sendMasterGive(uint64_t source, uint8_t slot, uint64_t target) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_LOOT_MASTER_GIVE));
    pkt.writeUInt64(source);
    pkt.writeUInt8(slot);
    pkt.writeUInt64(target);
    sender_.send(pkt);
}

void LootGateway::sendRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_LOOT_ROLL));
    pkt.writeUInt64(objectGuid);
    pkt.writeUInt32(slot);
    pkt.writeUInt8(rollType);
    sender_.send(pkt);
}

std::optional<loot::ILootWorld::ItemFacts> LootGateway::item(uint32_t itemId) const {
    const auto* info = inventory_.getItemInfo(itemId);
    if (!info) return std::nullopt;
    ItemFacts facts;
    facts.name = info->name;
    facts.quality = info->quality;
    facts.bindsOnPickup = info->valid && info->bindType == 1;
    return facts;
}

void LootGateway::requestItem(uint32_t itemId) {
    inventory_.ensureItemInfo(itemId);
}

std::string LootGateway::playerName(uint64_t guid) const {
    return world_.playerName(guid);
}

void LootGateway::creditMoney(uint32_t copper) {
    inventory_.creditLootedMoney(copper);
}

bool LootGateway::stillOpening(uint64_t guid) const {
    return world_.gameObjectLootPending(guid);
}

bool LootGateway::objectLootArrived(uint64_t guid) {
    return world_.gameObjectLootOpened(guid);
}

namespace {

/// The item as a chat link, named "Item #n" while its template has not come.
std::string itemLink(const InventoryHandler& inventory, uint32_t itemId, const std::string& suffix = {}) {
    const auto* info = inventory.getItemInfo(itemId);
    std::string name = (info && !info->name.empty()) ? info->name : "Item #" + std::to_string(itemId);
    if (!suffix.empty()) name += " " + suffix;
    return buildItemLink(itemId, info ? info->quality : 1u, name);
}

} // namespace

std::vector<core::EventBus::Subscription>
subscribeLootNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                     InventoryHandler& inventory, InventoryQueries& world, const core::CVarStore& cvars) {
    using namespace loot;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    // Carries whether this loot is being taken automatically. The loot frame
    // reads it on the path where it could not show itself, and closes with
    // CloseLoot(autoLoot == 0) - so an absent argument compares false and
    // tells the server the window opened when it did not.
    s.push_back(events.subscribe<LootOpened>([addon](const LootOpened& e) {
        addon("LOOT_OPENED", {e.autoLoot ? "1" : "0"});
        addon("LOOT_READY");
    }));
    s.push_back(events.subscribe<LootClosed>([addon](const auto&) { addon("LOOT_CLOSED"); }));
    s.push_back(events.subscribe<LootSlotCleared>([addon](const LootSlotCleared& e) {
        addon("LOOT_SLOT_CLEARED", {std::to_string(e.slot)});
    }));
    // The interface reads arg1 to know which button to refresh.
    s.push_back(events.subscribe<LootSlotChanged>([addon](const LootSlotChanged& e) {
        addon("LOOT_SLOT_CHANGED", {std::to_string(e.slot)});
    }));
    s.push_back(events.subscribe<BindConfirmRequested>([addon](const BindConfirmRequested& e) {
        addon("LOOT_BIND_CONFIRM", {std::to_string(e.slot)});
    }));
    // Both paths that learn about looted money end here - the server's notify
    // and the fallback for servers that do not send one - so the line and the
    // coin cannot differ between them. The coin's sound hears the same event.
    s.push_back(events.subscribe<MoneyAnnounced>([line](const MoneyAnnounced& e) {
        line("Looted: " + formatCopperAmount(e.copper));
    }));
    s.push_back(events.subscribe<PurseChanged>([addon](const auto&) { addon("PLAYER_MONEY"); }));
    // The candidate list arrives because the master looter picked an item, and
    // the menu that assigns it is built from it. One event opens the menu, one
    // fills it; they are the same frame.
    s.push_back(events.subscribe<MasterLootListReceived>([addon](const auto&) {
        addon("OPEN_MASTER_LOOT_LIST");
        addon("UPDATE_MASTER_LOOT_LIST");
    }));
    // Show Loot Spam, which the panel offers. Default on: it is what this
    // client has always printed.
    s.push_back(events.subscribe<LootItemReceived>([line, &inventory, &cvars](const LootItemReceived& e) {
        if (cvars.get("showLootSpam", "1") != "0")
            line("You receive loot: " + itemLink(inventory, e.itemId) + ".");
    }));

    // The roll id is what the interface asks every later question by, and
    // GroupLootFrame_OpenNewFrame sizes its countdown bar from the second
    // argument.
    s.push_back(events.subscribe<RollStarted>([line, addon](const RollStarted& e) {
        line("Loot roll started for " + buildItemLink(e.itemId, e.quality, e.itemName) + ".");
        addon("START_LOOT_ROLL", {std::to_string(e.rollId), std::to_string(e.countdownMs)});
    }));
    // Named, so the window that closes is the one this roll opened.
    s.push_back(events.subscribe<RollEnded>([addon](const RollEnded& e) {
        addon("CANCEL_LOOT_ROLL", {std::to_string(e.rollId)});
    }));
    // RollVote: 0 pass, 1 need, 2 greed, 3 disenchant.
    s.push_back(events.subscribe<VoteCast>([line](const VoteCast& e) {
        const char* what = e.rollType == 1 ? "rolled Need"
                         : e.rollType == 2 ? "rolled Greed"
                         : e.rollType == 3 ? "rolled Disenchant" : nullptr;
        if (what) line(e.playerName + " " + what + " - " + std::to_string(e.rollNumber));
        else line(e.playerName + " passed.");
    }));
    s.push_back(events.subscribe<EveryonePassed>([line, &inventory](const EveryonePassed& e) {
        line("Everyone passed on " + itemLink(inventory, e.itemId) + ".");
    }));
    s.push_back(events.subscribe<RollWon>([line, &inventory, &world](const RollWon& e) {
        const std::string suffix =
            e.randomPropertyId != 0 ? world.randomPropertyName(e.randomPropertyId) : std::string();
        // Pass cannot win.
        const char* how = e.rollType == 2 ? "Greed" : e.rollType == 3 ? "Disenchant" : "Need";
        line(e.winnerName + " won " + itemLink(inventory, e.itemId, suffix) + " (" + how + " - " +
             std::to_string(e.rollNumber) + ")");
    }));
    return s;
}

} // namespace wowee::game
