#include "game/action_bar_handler.hpp"
#include "game/account_data.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_events.hpp"
#include "game/game_utils.hpp"
#include "game/packet_sender.hpp"
#include "game/world_packets.hpp"
#include "core/logger.hpp"
#include "network/packet.hpp"

#include <string>

namespace wowee::game {

ActionBarHandler::ActionBarHandler(PacketSender& sender, FrameXmlEvents& frameXml,
                                   core::EventBus& events, ActionBarQueries& queries)
    : sender_(sender), frameXml_(frameXml), queries_(queries) {
    subscriptions_.push_back(events.subscribe<events::MacroDeleted>(
        [this](const events::MacroDeleted& e) { removeMacro(e.macroId); }));
}

void ActionBarHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_ACTION_BUTTONS] = [this](network::Packet& p) { handleActionButtons(p); };
}

void ActionBarHandler::setSlot(int slot, ActionBarSlot::Type type, uint32_t id) {
    if (slot < 0 || slot >= kActionBarSlots) {
        LOG_WARNING("Action bar: button ", slot + 1, " is not one of the ", kActionBarSlots,
                    "; nothing placed");
        return;
    }
    bar_[slot].type = type;
    bar_[slot].id = id;
    // So the button shows the item's name and icon rather than a placeholder.
    if (type == ActionBarSlot::ITEM && id != 0) queries_.requestItemInfo(id);
    frameXml_.fireFrameXmlEvent("ACTIONBAR_SLOT_CHANGED", {std::to_string(slot + 1)});
    frameXml_.fireFrameXmlEvent("ACTIONBAR_UPDATE_STATE", {});
    // The server keeps the bar; told now, it is there at the next login and
    // in every other client. Before the character is in the world there is
    // nothing on the server to store it against.
    if (sender_.characterInWorld()) {
        sender_.send(SetActionButtonPacket::build(static_cast<uint8_t>(slot),
                                                  static_cast<uint8_t>(type), id,
                                                  isClassicLikeExpansion()));
    }
}

void ActionBarHandler::clear() {
    bar_ = {};
}

void ActionBarHandler::tickCooldowns(float deltaTime) {
    for (auto& slot : bar_) {
        if (slot.cooldownRemaining > 0.0f) {
            slot.cooldownRemaining -= deltaTime;
            if (slot.cooldownRemaining < 0.0f) slot.cooldownRemaining = 0.0f;
        }
    }
}

void ActionBarHandler::handleActionButtons(network::Packet& packet) {
    // Slot encoding differs by expansion:
    //   Classic/Turtle: uint16 actionId + uint8 type + uint8 misc
    //     type: 0=spell, 1=item, 64=macro
    //   TBC/WotLK: uint32 packed = actionId | (type << 24)
    //     type: 0x00=spell, 0x80=item, 0x40=macro, 0x41=character macro
    // Format differences:
    //   Classic 1.12: no mode byte, 120 slots (480 bytes)
    //   TBC 2.4.3:    no mode byte, 132 slots (528 bytes)
    //   WotLK 3.3.5a: uint8 mode + 144 slots (577 bytes)
    //
    // The server's bar is the bar: every slot is set from the packet, an
    // empty one included.
    size_t rem = packet.getRemainingSize();
    const int serverBarSlots = isClassicLikeExpansion() ? 120
                             : isActiveExpansion("tbc") ? 132
                                                        : 144;
    if (isActiveExpansion("wotlk")) {
        if (rem < 1) {
            LOG_WARNING("SMSG_ACTION_BUTTONS: empty packet; the action bar is left as it was");
            return;
        }
        // 0 and 1 carry the whole bar (1 after a talent spec switch); 2
        // carries nothing and clears it, sent before a spec's spells are
        // unlearned and the next spec's bar follows.
        const uint8_t mode = packet.readUInt8();
        rem--;
        if (mode == 2) {
            bar_ = {};
            LOG_INFO("SMSG_ACTION_BUTTONS: bar cleared by the server");
            frameXml_.fireFrameXmlEvent("ACTIONBAR_SLOT_CHANGED", {"0"});
            packet.skipAll();
            return;
        }
    }
    if (rem < static_cast<size_t>(serverBarSlots) * 4) {
        LOG_WARNING("SMSG_ACTION_BUTTONS: ", rem, " bytes for ", serverBarSlots,
                    " buttons; the action bar is left as it was");
        packet.skipAll();
        return;
    }
    ActionBar bar{};
    for (int i = 0; i < serverBarSlots; ++i) {
        const uint32_t packed = packet.readUInt32();
        if (i >= kActionBarSlots || packed == 0) continue;
        uint8_t type = 0;
        uint32_t id = 0;
        if (isClassicLikeExpansion()) {
            id = packed & 0x0000FFFFu;
            type = static_cast<uint8_t>((packed >> 16) & 0xFF);
        } else {
            type = static_cast<uint8_t>((packed >> 24) & 0xFF);
            id = packed & 0x00FFFFFFu;
        }
        if (id == 0) continue;
        ActionBarSlot& slot = bar[i];
        switch (type) {
            case 0x00: slot.type = ActionBarSlot::SPELL; slot.id = id; break;
            case 0x01: slot.type = ActionBarSlot::ITEM;  slot.id = id; break;  // Classic item
            case 0x80: slot.type = ActionBarSlot::ITEM;  slot.id = id; break;  // TBC/WotLK item
            case 0x40: slot.type = ActionBarSlot::MACRO; slot.id = id; break;  // the account's macro
            // The character's macro: its id carries kCharacterMacroIdBase,
            // which 24 bits cannot hold, so the type says which half.
            case 0x41: slot.type = ActionBarSlot::MACRO; slot.id = kCharacterMacroIdBase | id; break;
            default:
                // Said once per kind: an equipment set on the bar is a button
                // this client cannot draw yet, and it is drawn empty every login.
                if (reportedTypes_.insert(type).second) {
                    LOG_WARNING("SMSG_ACTION_BUTTONS: button ", i + 1, " holds action type 0x",
                                std::hex, static_cast<int>(type), std::dec, " (id ", id,
                                "), which this client does not draw; that button is shown empty");
                }
                break;
        }
    }
    bar_ = bar;
    syncCooldowns();
    LOG_INFO("SMSG_ACTION_BUTTONS: populated action bar from server");
    // Zero means every slot, which is what the whole bar arriving at once is.
    // Sent with no argument the buttons compared nil against zero and against
    // their own action, matched neither, and the bar the server had just sent
    // drew nothing.
    frameXml_.fireFrameXmlEvent("ACTIONBAR_SLOT_CHANGED", {"0"});
    packet.skipAll();
}

void ActionBarHandler::syncCooldowns() {
    for (auto& slot : bar_) {
        float remaining = 0.0f;
        if (slot.type == ActionBarSlot::SPELL && slot.id != 0) {
            remaining = queries_.spellCooldownRemaining(slot.id);
        } else if (slot.type == ActionBarSlot::ITEM && slot.id != 0) {
            remaining = queries_.itemCooldownRemaining(slot.id);
        }
        if (remaining > 0.0f) {
            slot.cooldownRemaining = remaining;
            slot.cooldownTotal = remaining;
        }
    }
}

void ActionBarHandler::removeMacro(uint32_t macroId) {
    // A button left holding a macro that no longer exists would show a blank
    // and run nothing; the original client takes it off the bar.
    for (int slot = 0; slot < kActionBarSlots; ++slot) {
        if (bar_[slot].type == ActionBarSlot::MACRO && bar_[slot].id == macroId) {
            setSlot(slot, ActionBarSlot::EMPTY, 0);
        }
    }
}

} // namespace wowee::game
