#include "game/player_handler.hpp"

#include "game/character.hpp"
#include "game/chat_log.hpp"
#include "game/flat_field_map.hpp"
#include "game/frame_xml_events.hpp"
#include "game/game_events.hpp"
#include "game/opcodes.hpp"
#include "game/update_field_table.hpp"
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

PlayerHandler::PlayerHandler(FrameXmlEvents& frameXml, core::EventBus& events, ChatLog& chat,
                             PlayerQueries& queries, const StatTables& tables)
    : frameXml_(frameXml), events_(events), chat_(chat), queries_(queries), tables_(tables) {}

void PlayerHandler::registerOpcodes(DispatchTable& table) {
    for (auto op : {Opcode::SMSG_LEVELUP_INFO, Opcode::SMSG_LEVELUP_INFO_ALT}) {
        table[op] = [this](network::Packet& packet) { handleLevelUp(packet); };
    }
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

void PlayerHandler::restStateArrived(bool resting) {
    if (resting == sheet_.resting) return;
    sheet_.resting = resting;
    chat_.addSystemChatMessage(resting ? "You are now resting." : "You are no longer resting.");
    if (frameXml_.frameXmlListening()) frameXml_.fireFrameXmlEvent("PLAYER_UPDATE_RESTING", {});
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
