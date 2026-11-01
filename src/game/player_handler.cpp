#include "game/player_handler.hpp"

#include "game/character.hpp"
#include "game/chat_log.hpp"
#include "game/flat_field_map.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_events.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/protocol_constants.hpp"
#include "game/ui_errors.hpp"
#include "game/world_packets.hpp"
#include "game/update_field_table.hpp"
#include "core/coordinates.hpp"
#include "core/event_bus.hpp"
#include "core/logger.hpp"
#include "network/packet.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace wowee::game {

namespace {

/// Where the fields this reads sit, for the expansion in use. Resolved at each
/// update, because the expansion is chosen after the game is built.
struct PlayerFieldIndices {
    uint16_t xp, nextXp, restedXp, level, coinage, honor, arena, playerFlags, armor;
    uint16_t pBytes, pBytes2, chosenTitle;
    uint16_t stats[5];
    uint16_t meleeAP, rangedAP, spDmg1, healBonus;
    uint16_t blockPct, dodgePct, parryPct, critPct, rangedCritPct, sCrit1, rating1;
    uint16_t expertise, offhandExpertise, manaRegen, manaRegenCasting;

    static PlayerFieldIndices resolve() {
        return PlayerFieldIndices{
            .xp = fieldIndex(UF::PLAYER_XP),
            .nextXp = fieldIndex(UF::PLAYER_NEXT_LEVEL_XP),
            .restedXp = fieldIndex(UF::PLAYER_REST_STATE_EXPERIENCE),
            .level = fieldIndex(UF::UNIT_FIELD_LEVEL),
            .coinage = fieldIndex(UF::PLAYER_FIELD_COINAGE),
            .honor = fieldIndex(UF::PLAYER_FIELD_HONOR_CURRENCY),
            .arena = fieldIndex(UF::PLAYER_FIELD_ARENA_CURRENCY),
            .playerFlags = fieldIndex(UF::PLAYER_FLAGS),
            .armor = fieldIndex(UF::UNIT_FIELD_RESISTANCES),
            .pBytes = fieldIndex(UF::PLAYER_BYTES),
            .pBytes2 = fieldIndex(UF::PLAYER_BYTES_2),
            .chosenTitle = fieldIndex(UF::PLAYER_CHOSEN_TITLE),
            .stats = {fieldIndex(UF::UNIT_FIELD_STAT0), fieldIndex(UF::UNIT_FIELD_STAT1),
             fieldIndex(UF::UNIT_FIELD_STAT2), fieldIndex(UF::UNIT_FIELD_STAT3),
             fieldIndex(UF::UNIT_FIELD_STAT4)},
            .meleeAP = fieldIndex(UF::UNIT_FIELD_ATTACK_POWER),
            .rangedAP = fieldIndex(UF::UNIT_FIELD_RANGED_ATTACK_POWER),
            .spDmg1 = fieldIndex(UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS),
            .healBonus = fieldIndex(UF::PLAYER_FIELD_MOD_HEALING_DONE_POS),
            .blockPct = fieldIndex(UF::PLAYER_BLOCK_PERCENTAGE),
            .dodgePct = fieldIndex(UF::PLAYER_DODGE_PERCENTAGE),
            .parryPct = fieldIndex(UF::PLAYER_PARRY_PERCENTAGE),
            .critPct = fieldIndex(UF::PLAYER_CRIT_PERCENTAGE),
            .rangedCritPct = fieldIndex(UF::PLAYER_RANGED_CRIT_PERCENTAGE),
            .sCrit1 = fieldIndex(UF::PLAYER_SPELL_CRIT_PERCENTAGE1),
            .rating1 = fieldIndex(UF::PLAYER_FIELD_COMBAT_RATING_1),
            .expertise = fieldIndex(UF::PLAYER_EXPERTISE),
            .offhandExpertise = fieldIndex(UF::PLAYER_OFFHAND_EXPERTISE),
            // Mana is power index 0, so the flat modifier and its interrupted
            // (while-casting) twin sit at the base of each seven-wide array.
            .manaRegen = fieldIndex(UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER),
            .manaRegenCasting = fieldIndex(UF::UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER)
        };
    }
};

}  // namespace

PlayerHandler::PlayerHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                             ChatLog& chat, PlayerQueries& queries, const StatTables& tables)
    : sender_(sender), frameXml_(frameXml), events_(events), chat_(chat), queries_(queries),
      tables_(tables) {}

void PlayerHandler::registerOpcodes(DispatchTable& table) {
    for (auto op : {Opcode::SMSG_LEVELUP_INFO, Opcode::SMSG_LEVELUP_INFO_ALT}) {
        table[op] = [this](network::Packet& packet) { handleLevelUp(packet); };
    }
    table[Opcode::SMSG_STANDSTATE_UPDATE] = [this](network::Packet& packet) {
        handleStandStateUpdate(packet);
    };
    table[Opcode::SMSG_PLAYERBOUND] = [this](network::Packet& p) { handlePlayerBound(p); };
    table[Opcode::SMSG_BINDPOINTUPDATE] = [this](network::Packet& p) { handleBindPointUpdate(p); };
    // The innkeeper asking whether to make this the player's home. FrameXML's
    // gossip window follows the real flow: the server asks, the CONFIRM_BINDER
    // popup accepts, and confirmBinder answers.
    table[Opcode::SMSG_BINDER_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) {
            LOG_WARNING("SMSG_BINDER_CONFIRM too short: ", packet.getSize(),
                        " bytes; no \"make this your home?\" is asked");
            packet.skipAll();
            return;
        }
        sheet_.binderGuid = packet.readUInt64();
        // Who is asking: StaticPopup_Show("CONFIRM_BINDER", arg1) puts the name
        // into "Do you want to make <name> your new home?".
        frameXml_.fireFrameXmlEvent("CONFIRM_BINDER", {queries_.unitName(sheet_.binderGuid)});
    };
    table[Opcode::SMSG_SET_PROFICIENCY] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(5)) {
            LOG_WARNING("SMSG_SET_PROFICIENCY too short: ", packet.getSize(),
                        " bytes; what the character can use is not known");
            packet.skipAll();
            return;
        }
        const uint8_t itemClass = packet.readUInt8();
        const uint32_t mask = packet.readUInt32();
        if (itemClass == 2) sheet_.weaponProficiency = mask;
        else if (itemClass == 4) sheet_.armorProficiency = mask;
    };
    table[Opcode::SMSG_START_MIRROR_TIMER] = [this](network::Packet& p) { handleStartMirrorTimer(p); };
    table[Opcode::SMSG_STOP_MIRROR_TIMER] = [this](network::Packet& p) { handleStopMirrorTimer(p); };
    table[Opcode::SMSG_PAUSE_MIRROR_TIMER] = [this](network::Packet& p) { handlePauseMirrorTimer(p); };
    table[Opcode::SMSG_CONVERT_RUNE] = [this](network::Packet& p) { handleConvertRune(p); };
    table[Opcode::SMSG_RESYNC_RUNES] = [this](network::Packet& p) { handleResyncRunes(p); };
    table[Opcode::SMSG_ADD_RUNE_POWER] = [this](network::Packet& p) { handleAddRunePower(p); };
    table[Opcode::SMSG_ENABLE_BARBER_SHOP] = [this](network::Packet& /*packet*/) {
        LOG_INFO("SMSG_ENABLE_BARBER_SHOP: barber shop available");
        sheet_.barberShopOpen = true;
        frameXml_.fireFrameXmlEvent("BARBER_SHOP_OPEN", {});
    };
    table[Opcode::SMSG_BARBER_SHOP_RESULT] = [this](network::Packet& p) { handleBarberShopResult(p); };
}

// ---- Home ----

std::optional<HomeBind> PlayerHandler::homeBind() const {
    if (!sheet_.hasHomeBind) return std::nullopt;
    return HomeBind{sheet_.homeBindMapId, sheet_.homeBindPos};
}

void PlayerHandler::announceHome(const char* lead, uint32_t zoneId) {
    std::string message = lead;
    const std::string zoneName = queries_.areaName(zoneId);
    if (!zoneName.empty()) message += " to " + zoneName;
    message += '.';
    chat_.addSystemChatMessage(message);
}

void PlayerHandler::handlePlayerBound(network::Packet& packet) {
    if (!packet.hasRemaining(16)) {
        LOG_WARNING("SMSG_PLAYERBOUND too short: ", packet.getSize(), " bytes; the new home is not announced");
        packet.skipAll();
        return;
    }
    /*uint64_t binderGuid =*/ packet.readUInt64();
    const uint32_t mapId = packet.readUInt32();
    const uint32_t zoneId = packet.readUInt32();
    const bool changed = !sheet_.hasHomeBind || sheet_.homeBindMapId != mapId ||
                         sheet_.homeBindZoneId != zoneId;
    sheet_.homeBindMapId = mapId;
    sheet_.homeBindZoneId = zoneId;
    if (changed) announceHome("Your home location has been set", zoneId);
}

void PlayerHandler::handleBindPointUpdate(network::Packet& packet) {
    BindPointUpdateData data;
    if (!BindPointUpdateParser::parse(packet, data)) {
        LOG_WARNING("SMSG_BINDPOINTUPDATE too short: ", packet.getSize(),
                    " bytes; the hearthstone keeps the home it had");
        return;
    }
    const glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
    const bool wasSet = sheet_.hasHomeBind;
    const bool changed = !sheet_.hasHomeBind || sheet_.homeBindMapId != data.mapId ||
                         sheet_.homeBindZoneId != data.zoneId ||
                         glm::length(sheet_.homeBindPos - canonical) > 0.5f;
    sheet_.hasHomeBind = true;
    sheet_.homeBindMapId = data.mapId;
    sheet_.homeBindZoneId = data.zoneId;
    sheet_.homeBindPos = canonical;
    events_.publish(events::BindPointChanged{data.mapId, canonical.x, canonical.y, canonical.z});
    if (wasSet && changed) announceHome("Your home has been set", data.zoneId);
}

void PlayerHandler::confirmBinder() {
    if (!sender_.characterInWorld() || sheet_.binderGuid == 0) return;
    sender_.send(BinderActivatePacket::build(sheet_.binderGuid));
    LOG_INFO("Sent CMSG_BINDER_ACTIVATE (confirmed) for npc=0x", std::hex, sheet_.binderGuid, std::dec);
}

// ---- Mirror timers ----

const MirrorTimer& PlayerHandler::mirrorTimer(int type) const {
    static const MirrorTimer kNone;
    return (type >= 0 && type < 3) ? sheet_.mirrorTimers[static_cast<size_t>(type)] : kNone;
}

void PlayerHandler::handleStartMirrorTimer(network::Packet& packet) {
    // type(4) + value(4) + maxValue(4) + scale(4) + paused(1) + spellId(4).
    // The last two were being read the other way round: harmless while the
    // spell id is 0, but a non-zero one would land its high byte in paused and
    // freeze the bar.
    if (!packet.hasRemaining(21)) {
        LOG_WARNING("SMSG_START_MIRROR_TIMER too short: ", packet.getSize(), " bytes; no timer bar is shown");
        packet.skipAll();
        return;
    }
    const uint32_t type = packet.readUInt32();
    const int32_t value = static_cast<int32_t>(packet.readUInt32());
    const int32_t maxV = static_cast<int32_t>(packet.readUInt32());
    const int32_t scale = static_cast<int32_t>(packet.readUInt32());
    const uint8_t paused = packet.readUInt8();
    /*uint32_t spellId =*/ packet.readUInt32();
    if (type >= 3) {
        LOG_WARNING("SMSG_START_MIRROR_TIMER: timer type ", type, " is none of the three; no bar is shown");
        return;
    }
    auto& t = sheet_.mirrorTimers[type];
    t.value = value;
    t.maxValue = maxV;
    t.scale = scale;
    t.paused = (paused != 0);
    t.active = true;
    t.pendingMs = 0.0f;  // server re-sync; drop the local carry
    // Said once per timer, because nothing on this side decides any of it. The
    // length of a breath is the server's: three minutes plus whatever
    // SPELL_AURA_MOD_WATER_BREATHING adds, which for a Forsaken is 300% more
    // and so twelve. A maxValue of 180000 on an undead character means the
    // racial is not being applied there, and no change here can put it right.
    static bool saidTimer[3] = {false, false, false};
    if (!saidTimer[type]) {
        saidTimer[type] = true;
        LOG_INFO("Mirror timer ", kMirrorTimerNames[type], " from the server: value=", value,
                 "ms of max=", maxV, "ms scale=", scale, " paused=", (paused != 0 ? 1 : 0));
    }
    // MirrorTimer_Show(timer, value, maxvalue, scale, paused, label): named,
    // with a label, as the interface matches and draws it.
    frameXml_.fireFrameXmlEvent("MIRROR_TIMER_START",
                                {kMirrorTimerNames[type], std::to_string(value), std::to_string(maxV),
                                 std::to_string(scale), paused ? "1" : "0", kMirrorTimerLabels[type]});
}

void PlayerHandler::handleStopMirrorTimer(network::Packet& packet) {
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("SMSG_STOP_MIRROR_TIMER too short: ", packet.getSize(), " bytes; the timer bar stays");
        packet.skipAll();
        return;
    }
    const uint32_t type = packet.readUInt32();
    if (type >= 3) return;
    sheet_.mirrorTimers[type].active = false;
    sheet_.mirrorTimers[type].value = 0;
    // MirrorTimer_Hide matches on the timer's name, so a number hides nothing.
    frameXml_.fireFrameXmlEvent("MIRROR_TIMER_STOP", {kMirrorTimerNames[type]});
}

void PlayerHandler::handlePauseMirrorTimer(network::Packet& packet) {
    if (!packet.hasRemaining(5)) {
        LOG_WARNING("SMSG_PAUSE_MIRROR_TIMER too short: ", packet.getSize(), " bytes; the timer runs on");
        packet.skipAll();
        return;
    }
    const uint32_t type = packet.readUInt32();
    const uint8_t paused = packet.readUInt8();
    if (type >= 3) return;
    sheet_.mirrorTimers[type].paused = (paused != 0);
    frameXml_.fireFrameXmlEvent("MIRROR_TIMER_PAUSE", {paused ? "1" : "0"});
}

void PlayerHandler::tickMirrorTimers(float dt) {
    if (dt <= 0.0f) return;
    const float elapsedMs = dt * 1000.0f;
    for (size_t ti = 0; ti < sheet_.mirrorTimers.size(); ++ti) {
        auto& t = sheet_.mirrorTimers[ti];
        if (!t.active || t.paused || t.scale == 0 || t.maxValue <= 0) continue;
        // Carry the sub-millisecond remainder: truncating each frame would lose
        // most of a 144fps frame's 6.94ms and run the timer visibly slow.
        t.pendingMs += elapsedMs;
        const int32_t wholeMs = static_cast<int32_t>(t.pendingMs);
        if (wholeMs == 0) continue;
        t.pendingMs -= static_cast<float>(wholeMs);
        t.value = std::clamp(t.value + t.scale * wholeMs, 0, t.maxValue);
        // Refilled, so it is over. The server does not say so at this point:
        // on surfacing it sends one update with a positive scale and then
        // nothing until its own counter reaches full, several seconds later.
        // Only when refilling: a drowning bar reaching zero is when the damage
        // starts, and the server keeps it up. FrameXML's bar hides on
        // MIRROR_TIMER_STOP and nothing else, so it is told.
        if (t.scale > 0 && t.value >= t.maxValue) {
            t.active = false;
            t.pendingMs = 0.0f;
            frameXml_.fireFrameXmlEvent("MIRROR_TIMER_STOP", {kMirrorTimerNames[ti]});
        }
    }
}

// ---- Runes ----

// The rune bar draws itself from these two events and from nothing else.
// "usable" is sent as a present argument or no argument at all rather than as
// one and zero, because zero is true in Lua.
void PlayerHandler::announceRune(uint32_t index) {
    if (index >= sheet_.runes.size()) return;
    const std::string oneBased = std::to_string(index + 1);
    frameXml_.fireFrameXmlEvent("RUNE_TYPE_UPDATE", {oneBased});
    if (sheet_.runes[index].ready) {
        frameXml_.fireFrameXmlEvent("RUNE_POWER_UPDATE", {oneBased, "1"});
    } else {
        frameXml_.fireFrameXmlEvent("RUNE_POWER_UPDATE", {oneBased});
    }
}

void PlayerHandler::handleConvertRune(network::Packet& packet) {
    // uint8 runeIndex + uint8 newRuneType (0=Blood,1=Unholy,2=Frost,3=Death)
    if (!packet.hasRemaining(2)) {
        LOG_WARNING("SMSG_CONVERT_RUNE too short: ", packet.getSize(), " bytes; the rune keeps its type");
        packet.skipAll();
        return;
    }
    const uint8_t idx = packet.readUInt8();
    const uint8_t type = packet.readUInt8();
    if (idx >= sheet_.runes.size()) {
        LOG_WARNING("SMSG_CONVERT_RUNE: rune ", static_cast<int>(idx), " of six; nothing converted");
        return;
    }
    sheet_.runes[idx].type = static_cast<RuneType>(type & 0x3);
    // The rune bar redraws a rune on this event and on nothing else.
    frameXml_.fireFrameXmlEvent("RUNE_TYPE_UPDATE", {std::to_string(idx + 1)});
}

// uint32 count, then per rune: uint8 type, uint8 elapsed - Player::ResyncRunes.
// The server sends 255 - cooldown * 51, so 255 is a rune ready and 0 one just
// spent.
void PlayerHandler::handleResyncRunes(network::Packet& packet) {
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("SMSG_RESYNC_RUNES too short: ", packet.getSize(), " bytes; the runes are not resynced");
        packet.skipAll();
        return;
    }
    const uint32_t count = packet.readUInt32();
    for (uint32_t i = 0; i < count && i < sheet_.runes.size(); ++i) {
        if (!packet.hasRemaining(2)) {
            LOG_WARNING("SMSG_RESYNC_RUNES truncated at rune ", i, " of ", count, "; the rest keep their state");
            break;
        }
        const uint8_t type = packet.readUInt8();
        const uint8_t elapsed = packet.readUInt8();
        auto& rune = sheet_.runes[i];
        rune.type = static_cast<RuneType>(type & 0x3);
        rune.readyFraction = elapsed / 255.0f;
        rune.ready = (elapsed >= 255);
        announceRune(i);
    }
    packet.skipAll();
}

void PlayerHandler::handleAddRunePower(network::Packet& packet) {
    // uint32 runeMask (bit i=1 → rune i just became ready)
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("SMSG_ADD_RUNE_POWER too short: ", packet.getSize(), " bytes; no rune is made ready");
        packet.skipAll();
        return;
    }
    const uint32_t runeMask = packet.readUInt32();
    for (uint32_t i = 0; i < sheet_.runes.size(); ++i) {
        if ((runeMask & (1u << i)) == 0) continue;
        sheet_.runes[i].ready = true;
        sheet_.runes[i].readyFraction = 1.0f;
        announceRune(i);
    }
}

// ---- The barber's chair ----

void PlayerHandler::closeBarberShop() {
    sheet_.barberShopOpen = false;
    frameXml_.fireFrameXmlEvent("BARBER_SHOP_CLOSE", {});
}

void PlayerHandler::handleBarberShopResult(network::Packet& packet) {
    // uint32 result: 0=success, 1/3=not enough money, 2=not seated at barber
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("SMSG_BARBER_SHOP_RESULT too short: ", packet.getSize(),
                    " bytes; the player is not told how the haircut went");
        packet.skipAll();
        return;
    }
    const uint32_t result = packet.readUInt32();
    LOG_DEBUG("SMSG_BARBER_SHOP_RESULT: result=", result);
    if (result == 0) {
        chat_.addSystemChatMessage("Hairstyle changed.");
        closeBarberShop();
        return;
    }
    const char* msg = (result == 1 || result == 3) ? "Not enough money for new hairstyle."
                    : (result == 2) ? "You are not at a barber shop."
                    : "Barber shop unavailable.";
    raiseUiError(events_, frameXml_, msg);
    chat_.addSystemChatMessage(msg);
}

void PlayerHandler::handleStandStateUpdate(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    const uint8_t standState = packet.readUInt8();
    if (standState == sheet_.standState) return;
    sheet_.standState = standState;
    // 0=stand, 1=sit, 2-6=sit variants, 7=dead, 8=kneel. Logged because a
    // wrong state here is indistinguishable, in-game, from a wrong animation.
    // At warning level: the file log filters info out by default.
    LOG_WARNING("SMSG_STANDSTATE_UPDATE: standState=", static_cast<int>(standState));
    events_.publish(events::StandStateChanged{standState});
}

void PlayerHandler::requestStandState(uint8_t standState) {
    if (!sender_.characterInWorld()) {
        LOG_WARNING("Cannot change stand state: not in world or not connected");
        return;
    }
    sender_.send(StandStateChangePacket::build(standState));
    LOG_INFO("Changed stand state to: ", static_cast<int>(standState));
}

void PlayerHandler::unitFlagsArrived(uint32_t previous, uint32_t flags) {
    constexpr uint32_t UNIT_FLAG_STUNNED = 0x00040000;
    const bool wasStunned = (previous & UNIT_FLAG_STUNNED) != 0;
    bool nowStunned = (flags & UNIT_FLAG_STUNNED) != 0;
    // The server stuns the player for the logout countdown, to root them in
    // place - it sits them down in the same breath. That is a movement
    // restriction, not a stun: playing the stun animation over it leaves the
    // character slumped rather than seated. Clearing the flag is still
    // honoured, so a cancelled logout recovers.
    if (nowStunned && queries_.loggingOut()) nowStunned = false;
    if (wasStunned != nowStunned) events_.publish(events::StunStateChanged{nowStunned});
}

void PlayerHandler::visibilityFlagsArrived(uint8_t previous, uint8_t flags) {
    const bool wasStealthed = (previous & UNIT_VIS_FLAG_CREEP) != 0;
    const bool nowStealthed = (flags & UNIT_VIS_FLAG_CREEP) != 0;
    if (wasStealthed != nowStealthed) events_.publish(events::StealthStateChanged{nowStealthed});
}

void PlayerHandler::beginCharacter() {
    sheet_ = Sheet{};
}

// ---- SMSG_LEVELUP_INFO / SMSG_LEVELUP_INFO_ALT (shared body) ----
void PlayerHandler::handleLevelUp(network::Packet& packet) {
    // Server-authoritative level-up event.
    // WotLK layout: uint32 newLevel + uint32 hpDelta + uint32 manaDelta + 5x uint32 statDeltas
    if (packet.hasRemaining(4)) {
        uint32_t newLevel = packet.readUInt32();
        if (newLevel > 0) {
            // Parse stat deltas (WotLK layout has 7 more uint32s)
            struct {
                uint32_t hp = 0, mana = 0;
                uint32_t str = 0, agi = 0, sta = 0, intel = 0, spi = 0;
            } deltas;
            if (packet.hasRemaining(28)) {
                deltas.hp    = packet.readUInt32();
                deltas.mana  = packet.readUInt32();
                deltas.str   = packet.readUInt32();
                deltas.agi   = packet.readUInt32();
                deltas.sta   = packet.readUInt32();
                deltas.intel = packet.readUInt32();
                deltas.spi   = packet.readUInt32();
            }
            uint32_t oldLevel = sheet_.level;
            sheet_.level = std::max(sheet_.level, newLevel);
            // Update the character-list entry so the selection screen
            // shows the correct level if the player logs out and back.
            if (Character* listed = queries_.listedCharacter()) listed->level = sheet_.level;
            if (newLevel > oldLevel) {
                // The sound, the ding banner, the glow and the client's
                // own chat line all answer this. FrameXML's chat writes
                // its line from PLAYER_LEVEL_UP below instead.
                events_.publish(events::PlayerLevelUp{oldLevel, newLevel});
                // All nine, in the order the interface reads them:
                // level, health, power, talent points, then the five
                // stats. The chat frame tests the third against zero
                // to decide whether to mention mana, so sending only
                // the level was an error on every level gained - and
                // every one of these was already parsed above.
                //
                // Talent points are zero: the packet carries no such
                // delta, and zero is what stops the interface claiming
                // a point was gained.
                frameXml_.fireFrameXmlEvent("PLAYER_LEVEL_UP", {
                    std::to_string(newLevel),
                    std::to_string(deltas.hp),
                    std::to_string(deltas.mana),
                    "0",
                    std::to_string(deltas.str),
                    std::to_string(deltas.agi),
                    std::to_string(deltas.sta),
                    std::to_string(deltas.intel),
                    std::to_string(deltas.spi),
                });
            }
        }
    }
    packet.skipAll();
}

void PlayerHandler::fieldsUpdated(const FlatFieldMap& fields) {
    // Both are sorted, so a simple insert_or_assign per key keeps the
    // invariant intact.
    for (const auto& [key, val] : fields) sheet_.fields.insert_or_assign(key, val);
}

uint32_t PlayerHandler::tableLevel() const {
    return std::min(sheet_.level, StatTables::kMaxLevel);
}

float PlayerHandler::critFromStat(StatTables::Table base, StatTables::Table perStat, int statIdx) const {
    const uint8_t cls = queries_.playerClass();
    // Classic has no game tables: its sheet has no crit from a stat to show.
    if (cls == 0 || !tables_.has(base) || !tables_.has(perStat)) return 0.0f;
    const float stat = static_cast<float>(std::max(0, sheet_.stats[statIdx]));
    return (tables_.perClass(base, cls) + stat * tables_.perClassAndLevel(perStat, cls, tableLevel())) *
           100.0f;  // the sheet reads a percent
}

float PlayerHandler::meleeCritFromAgility() const {
    return critFromStat(StatTables::Table::MeleeCritBase, StatTables::Table::MeleeCritPerAgility,
                        1 /* STAT_AGILITY */);
}

float PlayerHandler::spellCritFromIntellect() const {
    return critFromStat(StatTables::Table::SpellCritBase, StatTables::Table::SpellCritPerIntellect,
                        3 /* STAT_INTELLECT */);
}

float PlayerHandler::healthRegenFromSpirit() const {
    const uint8_t cls = queries_.playerClass();
    if (cls == 0 || !tables_.has(StatTables::Table::OctRegenHp) ||
        !tables_.has(StatTables::Table::RegenHpPerSpirit)) {
        return 0.0f;
    }
    const float spirit = static_cast<float>(std::max(0, sheet_.stats[4 /* SPIRIT */]));
    const float baseSpirit = std::min(spirit, 50.0f);
    const float moreSpirit = spirit - baseSpirit;
    return (baseSpirit * tables_.perClassAndLevel(StatTables::Table::OctRegenHp, cls, tableLevel()) +
            moreSpirit * tables_.perClassAndLevel(StatTables::Table::RegenHpPerSpirit, cls, tableLevel())) *
           2.0f;
}

float PlayerHandler::manaRegenFromSpirit() const {
    const uint8_t cls = queries_.playerClass();
    if (cls == 0 || !tables_.has(StatTables::Table::RegenMpPerSpirit)) return 0.0f;
    const float spirit = static_cast<float>(std::max(0, sheet_.stats[4 /* SPIRIT */]));
    return spirit * tables_.perClassAndLevel(StatTables::Table::RegenMpPerSpirit, cls, tableLevel());
}

float PlayerHandler::combatRatingBonus(int cr) const {
    const uint8_t cls = queries_.playerClass();
    if (cls == 0 || cr < 0 || cr >= static_cast<int>(StatTables::kRatings)) return 0.0f;
    if (!tables_.has(StatTables::Table::CombatRatings)) return 0.0f;  // Classic: no ratings
    const float coef = tables_.ratingAtLevel(static_cast<uint32_t>(cr), tableLevel());
    if (coef == 0.0f) return 0.0f;
    const float ratingValue = static_cast<float>(std::max(0, combatRating(cr)));
    // The class scalar came with WotLK; TBC divides by the coefficient alone.
    const float scalar = tables_.has(StatTables::Table::ClassRatingScalar)
        ? tables_.classRatingScalar(cls, static_cast<uint32_t>(cr))
        : 1.0f;
    return ratingValue * (scalar / coef);
}

void PlayerHandler::shapeshiftFormArrived(uint8_t form, FrameXmlEvents& packetEvents) {
    if (form == sheet_.shapeshiftForm) return;
    sheet_.shapeshiftForm = form;
    LOG_INFO("Shapeshift form changed: ", static_cast<int>(form));
    packetEvents.fireFrameXmlEvent("UPDATE_SHAPESHIFT_FORM", {});
    packetEvents.fireFrameXmlEvent("UPDATE_SHAPESHIFT_FORMS", {});
    // The stance bar and the action bar are two different
    // things. These two move the stance bar; the extra action
    // bar a form brings with it is shown and hidden by
    // UPDATE_BONUS_ACTIONBAR, and actionbutton.lua repages on
    // the same event. Neither was fired, so the bar never
    // appeared and the buttons kept reading page one.
    packetEvents.fireFrameXmlEvent("UPDATE_BONUS_ACTIONBAR", {});
    packetEvents.fireFrameXmlEvent("ACTIONBAR_PAGE_CHANGED", {});
}

void PlayerHandler::playerFieldsArrived(const FlatFieldMap& fields, bool isCreate,
                                        FrameXmlEvents& packetEvents) {
    const PlayerFieldIndices pfi = PlayerFieldIndices::resolve();
    Sheet& s = sheet_;
    // Whether anything the character sheet prints actually moved. The sheet
    // refreshes on events, not on a timer, so a stat that changes without one
    // being fired leaves the panel showing the old number until something else
    // happens to redraw it.
    bool statsChanged = false;
    bool ratingsChanged = false;
    bool powerChanged = false;
    bool rangedPowerChanged = false;
    bool resistancesChanged = false;
    bool spellBonusChanged = false;
    for (const auto& [key, val] : fields) {
        if (key == pfi.xp) {
            s.xp = val;
            if (!isCreate) {
                LOG_DEBUG("XP updated: ", val);
                packetEvents.fireFrameXmlEvent("PLAYER_XP_UPDATE", {std::to_string(val)});
            }
        }
        else if (key == pfi.nextXp) {
            s.nextLevelXp = val;
            if (!isCreate) LOG_DEBUG("Next level XP updated: ", val);
        }
        else if (pfi.restedXp != 0xFFFF && key == pfi.restedXp) {
            s.restedXp = val;
            if (!isCreate) packetEvents.fireFrameXmlEvent("UPDATE_EXHAUSTION", {});
        }
        else if (key == pfi.level) {
            s.level = val;
            if (!isCreate) LOG_DEBUG("Level updated: ", val);
            if (Character* listed = queries_.listedCharacter()) listed->level = val;
        }
        else if (key == pfi.coinage) {
            queries_.moneyFieldArrived(val, packetEvents);
        }
        else if (pfi.honor != 0xFFFF && key == pfi.honor) {
            s.honorPoints = val;
            LOG_DEBUG("Honor points ", isCreate ? "from update fields: " : "updated: ", val);
        }
        else if (pfi.arena != 0xFFFF && key == pfi.arena) {
            s.arenaPoints = val;
            LOG_DEBUG("Arena points ", isCreate ? "from update fields: " : "updated: ", val);
        }
        else if (pfi.armor != 0xFFFF && key == pfi.armor) {
            const int32_t armorVal = static_cast<int32_t>(val);
            if (s.armor != armorVal) resistancesChanged = true;
            s.armor = armorVal;
            if (isCreate) LOG_DEBUG("Armor rating from update fields: ", s.armor);
        }
        else if (pfi.armor != 0xFFFF && key > pfi.armor && key <= pfi.armor + 6) {
            const int32_t resVal = static_cast<int32_t>(val);
            // Armor is the first of the seven - pfi.armor is
            // UNIT_FIELD_RESISTANCES and the six schools follow it - which is
            // why one event covers the block.
            if (s.resistances[key - pfi.armor - 1] != resVal) resistancesChanged = true;
            s.resistances[key - pfi.armor - 1] = resVal;
        }
        else if (pfi.pBytes2 != 0xFFFF && key == pfi.pBytes2) {
            uint8_t bankBagSlots = static_cast<uint8_t>((val >> 16) & 0xFF);
            queries_.bankBagSlotsArrived(bankBagSlots);
            // Byte 3 (bits 24-31): REST_STATE
            // 0 = not resting, 1 = REST_TYPE_IN_TAVERN, 2 = REST_TYPE_IN_CITY
            uint8_t restStateByte = static_cast<uint8_t>((val >> 24) & 0xFF);
            if (isCreate) {
                LOG_DEBUG("PLAYER_BYTES_2 (CREATE): raw=0x", std::hex, val, std::dec,
                           " bankBagSlots=", static_cast<int>(bankBagSlots));
                bool wasResting = s.resting;
                s.resting = (restStateByte != 0);
                if (s.resting != wasResting) {
                    packetEvents.fireFrameXmlEvent("UPDATE_EXHAUSTION", {});
                    packetEvents.fireFrameXmlEvent("PLAYER_UPDATE_RESTING", {});
                }
            } else {
                // Byte 0 (bits 0-7): facial hair / piercings
                uint8_t facialHair = static_cast<uint8_t>(val & 0xFF);
                if (Character* listed = queries_.listedCharacter()) listed->facialFeatures = facialHair;
                LOG_DEBUG("PLAYER_BYTES_2 (VALUES): raw=0x", std::hex, val, std::dec,
                           " bankBagSlots=", static_cast<int>(bankBagSlots),
                           " facial=", static_cast<int>(facialHair));
                s.resting = (restStateByte != 0);
                events_.publish(events::PlayerAppearanceChanged{});
                events_.publish(events::PlayerModelRebuildRequested{});
            }
        }
        else if (pfi.chosenTitle != 0xFFFF && key == pfi.chosenTitle) {
            LOG_DEBUG("PLAYER_CHOSEN_TITLE ", isCreate ? "from update fields: " : "updated: ",
                      static_cast<int32_t>(val));
            queries_.chosenTitleArrived(static_cast<int32_t>(val));
        }
        // VALUES-only fields: PLAYER_BYTES (appearance) and PLAYER_FLAGS (ghost state)
        else if (!isCreate && pfi.pBytes != 0xFFFF && key == pfi.pBytes) {
            // PLAYER_BYTES changed (barber shop, polymorph, etc.)
            if (Character* listed = queries_.listedCharacter()) listed->appearanceBytes = val;
            events_.publish(events::PlayerAppearanceChanged{});
            events_.publish(events::PlayerModelRebuildRequested{});
        }
        else if (key == pfi.playerFlags) {
          // Resting is read on create as well as on change, because a player
          // who logs in inside an inn is already resting and there is no later
          // transition to notice. The ghost handling below stays on updates
          // only: it drives release and resurrection, which are transitions.
          constexpr uint32_t PLAYER_FLAGS_RESTING = 0x00000020;
          const bool nowResting = (val & PLAYER_FLAGS_RESTING) != 0;
          if (nowResting != s.resting) {
              s.resting = nowResting;
              packetEvents.fireFrameXmlEvent("PLAYER_UPDATE_RESTING", {});
          }
          // The helm and cloak switches, which the realm keeps and this client
          // never read back. Both are set by CMSG_SHOWING_HELM/CLOAK and come
          // home in PLAYER_FLAGS, so the realm had them right the whole time -
          // the switch simply started true every session and was written by
          // nothing but the toggle. Hide the helm, log out, and it was back.
          //
          // On create as well as on change, for the reason resting is: the
          // flags arrive with the player and there is no later transition.
          //
          // The flag says *hide*, so it is inverted here.
          {
            constexpr uint32_t PLAYER_FLAGS_HIDE_HELM  = 0x00000400;
            constexpr uint32_t PLAYER_FLAGS_HIDE_CLOAK = 0x00000800;
            const bool helm  = (val & PLAYER_FLAGS_HIDE_HELM) == 0;
            const bool cloak = (val & PLAYER_FLAGS_HIDE_CLOAK) == 0;
            if (helm != s.helmVisible || cloak != s.cloakVisible) {
                s.helmVisible = helm;
                s.cloakVisible = cloak;
                // The same rebuild the toggle asks for: the flag on its own
                // changes nothing on screen, and the hair under a helm has to
                // come back with it.
                queries_.markOnlineEquipmentDirty();
                LOG_INFO("PLAYER_FLAGS: helm ", helm ? "shown" : "hidden",
                         ", cloak ", cloak ? "shown" : "hidden");
            }
          }
          queries_.playerFlagsArrived(val, packetEvents);
          {
            // Which unit's flags, because that is the only thing the
            // handlers do with it: TargetFrame compares arg1 against its
            // own unit before redrawing the away and busy markers, so an
            // absent one matched no frame and none of them ever redrew.
            // The event only where something changed. A create block is the
            // interface being told what is, not what moved, and every frame
            // reads the current values when it is built.
            if (!isCreate) packetEvents.fireFrameXmlEvent("PLAYER_FLAGS_CHANGED", {"player"});
          }
        }
        else if (pfi.meleeAP  != 0xFFFF && key == pfi.meleeAP)  {
            const int32_t ap = static_cast<int32_t>(val);
            if (s.meleeAP != ap) powerChanged = true;
            s.meleeAP = ap;
        }
        else if (pfi.rangedAP != 0xFFFF && key == pfi.rangedAP) {
            const int32_t ap = static_cast<int32_t>(val);
            // Its own flag, because its own event is the one that lands.
            if (s.rangedAP != ap) rangedPowerChanged = true;
            s.rangedAP = ap;
        }
        else if (pfi.spDmg1   != 0xFFFF && key >= pfi.spDmg1 && key < pfi.spDmg1 + 7) {
            const int32_t dmgVal = static_cast<int32_t>(val);
            if (s.spellDmgBonus[key - pfi.spDmg1] != dmgVal) spellBonusChanged = true;
            s.spellDmgBonus[key - pfi.spDmg1] = dmgVal;
        }
        else if (pfi.healBonus != 0xFFFF && key == pfi.healBonus) {
            const int32_t healVal = static_cast<int32_t>(val);
            if (s.healBonus != healVal) spellBonusChanged = true;
            s.healBonus = healVal;
        }
        // Percentage stats are stored as IEEE 754 floats packed into uint32 update fields.
        // memcpy reinterprets the bits; clamp to [0..100] to guard against NaN/Inf from
        // corrupted packets reaching the UI (display-only, no gameplay logic depends on these).
        // Points, not a percentage, so read straight rather than through the
        // float reinterpretation the lines below need.
        // Mana regen per second, sent already computed by the server (spirit,
        // intellect, mp5 from gear and any while-casting talent are all folded
        // in) as float bits, one figure while not casting and one during the
        // five-second rule. A change refreshes the stat panel like any stat.
        else if (pfi.manaRegen != 0xFFFF && key == pfi.manaRegen) {
            float mr; std::memcpy(&mr, &val, 4);
            if (s.manaRegen != mr) statsChanged = true;
            s.manaRegen = mr;
        }
        else if (pfi.manaRegenCasting != 0xFFFF && key == pfi.manaRegenCasting) {
            float mr; std::memcpy(&mr, &val, 4);
            if (s.manaRegenCasting != mr) statsChanged = true;
            s.manaRegenCasting = mr;
        }
        else if (pfi.expertise != 0xFFFF && key == pfi.expertise) { s.expertise = static_cast<int32_t>(val); }
        else if (pfi.offhandExpertise != 0xFFFF && key == pfi.offhandExpertise) { s.offhandExpertise = static_cast<int32_t>(val); }
        else if (pfi.blockPct != 0xFFFF && key == pfi.blockPct) { std::memcpy(&s.blockPct, &val, 4); s.blockPct = std::clamp(s.blockPct, 0.0f, 100.0f); }
        else if (pfi.dodgePct != 0xFFFF && key == pfi.dodgePct) { std::memcpy(&s.dodgePct, &val, 4); s.dodgePct = std::clamp(s.dodgePct, 0.0f, 100.0f); }
        else if (pfi.parryPct != 0xFFFF && key == pfi.parryPct) { std::memcpy(&s.parryPct, &val, 4); s.parryPct = std::clamp(s.parryPct, 0.0f, 100.0f); }
        else if (pfi.critPct  != 0xFFFF && key == pfi.critPct)  { std::memcpy(&s.critPct,  &val, 4); s.critPct  = std::clamp(s.critPct,  0.0f, 100.0f); }
        else if (pfi.rangedCritPct != 0xFFFF && key == pfi.rangedCritPct) { std::memcpy(&s.rangedCritPct, &val, 4); s.rangedCritPct = std::clamp(s.rangedCritPct, 0.0f, 100.0f); }
        else if (pfi.sCrit1   != 0xFFFF && key >= pfi.sCrit1 && key < pfi.sCrit1 + 7) {
            std::memcpy(&s.spellCritPct[key - pfi.sCrit1], &val, 4);
        }
        else if (pfi.rating1  != 0xFFFF && key >= pfi.rating1 && key < pfi.rating1 + 25) {
            const int32_t r = static_cast<int32_t>(val);
            if (s.combatRatings[key - pfi.rating1] != r) ratingsChanged = true;
            s.combatRatings[key - pfi.rating1] = r;
        }
        else {
            for (int si = 0; si < 5; ++si) {
                if (pfi.stats[si] != 0xFFFF && key == pfi.stats[si]) {
                    const int32_t sv = static_cast<int32_t>(val);
                    if (s.stats[si] != sv) statsChanged = true;
                    s.stats[si] = sv;
                    break;
                }
            }
        }
        // Do not synthesize quest-log entries from raw update-field slots.
        // Slot layouts differ on some classic-family realms and can produce
        // phantom "already accepted" quests that block quest acceptance.
    }

    // Announced only on an update, never on the create block: at create the
    // sheet has not been built yet, and every field looks like a change.
    //
    // PaperDollFrame listens for these and calls PaperDollFrame_UpdateStats
    // from each. Without them the panels were filled once at login and then
    // never again - a new weapon or a stat buff left the old numbers on screen
    // with nothing to say they were stale.
    if (!isCreate) {
        if (statsChanged)   packetEvents.fireFrameXmlEvent("UNIT_STATS", {"player"});
        if (ratingsChanged) packetEvents.fireFrameXmlEvent("COMBAT_RATING_UPDATE", {});
        if (powerChanged)   packetEvents.fireFrameXmlEvent("UNIT_ATTACK_POWER", {"player"});
        // The ranged half separately, because the two are not
        // interchangeable to the sheet. PaperDollFrame registers
        // UNIT_ATTACK_POWER and then handles it nowhere - it is absent from
        // the branch that calls PaperDollFrame_UpdateStats and from every
        // other branch in the file, in this FrameXML and in the pet one. So
        // sending it for a ranged change refreshed nothing at all: a hunter
        // could gain or lose ranged attack power and the character sheet went
        // on showing the old number.
        //
        // UNIT_RANGED_ATTACK_POWER is in that branch, and is what the value
        // actually changed, so it is both the effective event and the true
        // one.
        if (rangedPowerChanged) {
            packetEvents.fireFrameXmlEvent("UNIT_RANGED_ATTACK_POWER", {"player"});
        }
        // Armor with the six resistances behind it, and the spell power and
        // healing bonus. The paperdoll refreshes its resistance panel from
        // UNIT_RESISTANCES alone - UNIT_DEFENSE is the *pet* sheet's event, not
        // this one - and its spell-power panel shares a branch with UNIT_STATS,
        // which only fires when one of the five base stats moves. So a spell
        // power buff that touched no stat, or a resistance aura, left the old
        // number on the sheet until the next login.
        if (resistancesChanged) packetEvents.fireFrameXmlEvent("UNIT_RESISTANCES", {"player"});
        if (spellBonusChanged)  packetEvents.fireFrameXmlEvent("PLAYER_DAMAGE_DONE_MODS", {"player"});
    }
}

} // namespace wowee::game
