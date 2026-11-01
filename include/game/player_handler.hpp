#pragma once

#include "game/flat_field_map.hpp"
#include "game/home_bind.hpp"
#include "game/opcode_table.hpp"
#include "game/stat_tables.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace wowee::core { class EventBus; }
namespace wowee::network { class Packet; }

namespace wowee::game {

class ChatLog;
class FrameXmlEvents;
class PacketSender;
struct Character;

/// The three timers the server runs on the player - exhaustion, breath and
/// feign death, in the wire's order.
///
/// The names are what MirrorTimer_Show and MirrorTimer_Hide match on, and the
/// labels what they draw beside the bar: mirrortimer.lua's four keys, of which
/// the server uses three.
inline constexpr const char* kMirrorTimerNames[3] = {"EXHAUSTION", "BREATH", "FEIGNDEATH"};
inline constexpr const char* kMirrorTimerLabels[3] = {"Exhaustion", "Breath", "Feign Death"};

struct MirrorTimer {
    int32_t value    = 0;
    int32_t maxValue = 0;
    int32_t scale    = 0;      ///< +1 counting up, -1 counting down
    bool    paused   = false;
    bool    active   = false;
    float   pendingMs = 0.0f;  ///< sub-millisecond carry for the local countdown
};

/// A death knight's rune: 0-1 Blood, 2-3 Unholy, 4-5 Frost until converted.
enum class RuneType : uint8_t { Blood = 0, Unholy = 1, Frost = 2, Death = 3 };
struct RuneSlot {
    RuneType type = RuneType::Blood;
    bool     ready = true;          ///< as the server last confirmed
    float    readyFraction = 1.0f;  ///< 0 just spent, 1 ready
};

/// What PlayerHandler asks of the rest of the game: the character list's
/// entry for the one in the world, and the parts of the player's fields whose
/// meaning another part of the game owns. Those are handed on as the fields
/// are read, so the interface hears of one update in the order of its fields.
class PlayerQueries {
public:
    virtual ~PlayerQueries() = default;
    /// The character list's entry for the character in the world, or null.
    virtual Character* listedCharacter() = 0;
    /// The class of the character in the world, 0 before it is known.
    [[nodiscard]] virtual uint8_t playerClass() const = 0;
    /// PLAYER_FIELD_COINAGE, for the inventory.
    virtual void moneyFieldArrived(uint32_t copper, FrameXmlEvents& packetEvents) = 0;
    /// The bank bag slots bought, from PLAYER_BYTES_2, for the inventory.
    virtual void bankBagSlotsArrived(uint8_t slots) = 0;
    /// PLAYER_CHOSEN_TITLE, for the titles.
    virtual void chosenTitleArrived(int32_t title) = 0;
    /// PLAYER_FLAGS, for what combat reads of death and the ghost.
    virtual void playerFlagsArrived(uint32_t flags, FrameXmlEvents& packetEvents) = 0;
    /// The helm or cloak was shown or hidden: the equipment is drawn again.
    virtual void markOnlineEquipmentDirty() = 0;
    /// The logout countdown is running.
    [[nodiscard]] virtual bool loggingOut() const = 0;
    /// AreaTable.dbc's name for a zone, empty when there is none.
    [[nodiscard]] virtual std::string areaName(uint32_t areaId) const = 0;
    /// The name a unit or player in the name cache goes by.
    [[nodiscard]] virtual std::string unitName(uint64_t guid) const = 0;
};

/// The character in the world as the server describes it: experience, level,
/// rest, PvP currency, the character sheet's numbers, the helm and cloak
/// switches, the shapeshift form, and how the character stands: sitting,
/// stunned, stealthed. Its home and the innkeeper offering a new one, the
/// weapons and armour it can use, its breath and the other mirror timers, a
/// death knight's runes and the barber's chair. The server is authoritative
/// for all of it; this keeps what it last said and tells the interface what
/// moved.
class PlayerHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    PlayerHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                  ChatLog& chat, PlayerQueries& queries, const StatTables& tables);

    void registerOpcodes(DispatchTable& table);

    /// A character is entering the world: nothing of the last one's sheet.
    /// A create block leaves out every field that is zero, so what is not
    /// cleared here would be kept.
    void beginCharacter();

    /// Every field the server has said of the player's object: what the
    /// create block held, with each update since laid over it.
    [[nodiscard]] const FlatFieldMap& fields() const { return sheet_.fields; }
    /// The player's object was created with these fields: all there is.
    void fieldsCreated(const FlatFieldMap& fields) { sheet_.fields = fields; }
    /// These of the player's fields moved.
    void fieldsUpdated(const FlatFieldMap& fields);

    /// The player's own fields: at create (`isCreate`) what is, in an update
    /// what moved. Events go into `packetEvents`, with the packet's others.
    void playerFieldsArrived(const FlatFieldMap& fields, bool isCreate, FrameXmlEvents& packetEvents);
    /// The form, the top byte of UNIT_FIELD_BYTES_1, in an update.
    void shapeshiftFormArrived(uint8_t form, FrameXmlEvents& packetEvents);
    /// UNIT_FIELD_FLAGS on the player, in an update: UNIT_FLAG_STUNNED.
    void unitFlagsArrived(uint32_t previous, uint32_t flags);
    /// The visibility byte of UNIT_FIELD_BYTES_1 on the player, in an update:
    /// UNIT_VIS_FLAG_CREEP is stealth.
    void visibilityFlagsArrived(uint8_t previous, uint8_t flags);
    /// Ask the server to sit, stand, kneel...: CMSG_STANDSTATECHANGE. The
    /// state changes when it answers.
    void requestStandState(uint8_t standState);
    /// The helm and cloak switches, as the player sets them.
    void showHelm(bool shown) { sheet_.helmVisible = shown; }
    void showCloak(bool shown) { sheet_.cloakVisible = shown; }

    [[nodiscard]] uint32_t xp() const { return sheet_.xp; }
    [[nodiscard]] uint32_t nextLevelXp() const { return sheet_.nextLevelXp; }
    [[nodiscard]] uint32_t restedXp() const { return sheet_.restedXp; }
    [[nodiscard]] bool isResting() const { return sheet_.resting; }
    [[nodiscard]] uint32_t level() const { return sheet_.level; }

    // PvP currency (TBC/WotLK only)
    [[nodiscard]] uint32_t honorPoints() const { return sheet_.honorPoints; }
    [[nodiscard]] uint32_t arenaPoints() const { return sheet_.arenaPoints; }

    // Server-authoritative armor (UNIT_FIELD_RESISTANCES[0])
    [[nodiscard]] int32_t armor() const { return sheet_.armor; }
    // Server-authoritative elemental resistances (UNIT_FIELD_RESISTANCES[1-6]).
    // school: 1=Holy, 2=Fire, 3=Nature, 4=Frost, 5=Shadow, 6=Arcane. Returns 0 if not received.
    [[nodiscard]] int32_t resistance(int school) const {
        if (school < 1 || school > 6) return 0;
        return sheet_.resistances[school - 1];
    }
    // Server-authoritative primary stats (UNIT_FIELD_STAT0-4: STR, AGI, STA, INT, SPI).
    // Returns -1 if the server hasn't sent the value yet.
    [[nodiscard]] int32_t stat(int idx) const {
        if (idx < 0 || idx > 4) return -1;
        return sheet_.stats[idx];
    }

    // Server-authoritative attack power (WotLK: UNIT_FIELD_ATTACK_POWER / RANGED).
    // Returns -1 if not yet received.
    [[nodiscard]] int32_t meleeAttackPower() const { return sheet_.meleeAP; }
    [[nodiscard]] int32_t rangedAttackPower() const { return sheet_.rangedAP; }

    // Server-authoritative spell damage / healing bonus (WotLK: PLAYER_FIELD_MOD_*).
    // spellPower returns the max damage bonus across magic schools 1-6 (Holy/Fire/Nature/Frost/Shadow/Arcane).
    // Returns -1 if not yet received.
    [[nodiscard]] int32_t spellPower() const {
        int32_t sp = -1;
        for (int i = 1; i <= 6; ++i) {
            if (sheet_.spellDmgBonus[i] > sp) sp = sheet_.spellDmgBonus[i];
        }
        return sp;
    }
    [[nodiscard]] int32_t healingPower() const { return sheet_.healBonus; }

    // Server-authoritative combat chance percentages (WotLK: PLAYER_* float fields).
    // Returns -1.0f if not yet received.
    [[nodiscard]] float dodgePct() const { return sheet_.dodgePct; }
    [[nodiscard]] float parryPct() const { return sheet_.parryPct; }
    [[nodiscard]] float blockPct() const { return sheet_.blockPct; }
    [[nodiscard]] float critPct() const { return sheet_.critPct; }
    [[nodiscard]] float rangedCritPct() const { return sheet_.rangedCritPct; }
    // Spell crit by school (0=Physical,1=Holy,2=Fire,3=Nature,4=Frost,5=Shadow,6=Arcane)
    [[nodiscard]] float spellCritPct(int school = 1) const {
        if (school < 0 || school > 6) return -1.0f;
        return sheet_.spellCritPct[school];
    }
    /// Expertise in points, main hand and off hand. Zero until the server
    /// says otherwise, which is also the right answer for a class that has
    /// none - unlike the percentages above, where -1 has to mean "not told".
    [[nodiscard]] int32_t expertise() const { return sheet_.expertise; }
    [[nodiscard]] int32_t offhandExpertise() const { return sheet_.offhandExpertise; }
    /// Mana regen per second, not-casting and while-casting - the server sends
    /// both already computed. GetManaRegen returns them for the mana-regen stat.
    [[nodiscard]] float manaRegen() const { return sheet_.manaRegen; }
    [[nodiscard]] float manaRegenCasting() const { return sheet_.manaRegenCasting; }

    // Server-authoritative combat ratings (WotLK: PLAYER_FIELD_COMBAT_RATING_1+idx).
    // Returns -1 if not yet received. Indices match AzerothCore CombatRating enum.
    [[nodiscard]] int32_t combatRating(int cr) const {
        if (cr < 0 || cr > 24) return -1;
        return sheet_.combatRatings[cr];
    }

    /// Melee critical-strike chance the player's Agility grants, as a percent,
    /// the way the character sheet's stat flyout reads it: the class base plus
    /// Agility times the per-class-per-level ratio, both from the combat game
    /// tables. Zero until the class is known.
    [[nodiscard]] float meleeCritFromAgility() const;
    /// Spell critical-strike chance the player's Intellect grants, as a percent -
    /// the spell-side twin of meleeCritFromAgility, read the same way from the
    /// spell-crit game tables.
    [[nodiscard]] float spellCritFromIntellect() const;
    /// The percent a combat rating grants - dodge, parry, haste, defense and the
    /// rest of the rating-based stats the flyout shows. rating × (class scalar ÷
    /// the level's coefficient), from gtCombatRatings and the class scalar table,
    /// exactly as AzerothCore's GetRatingBonusValue reads it. Zero for an
    /// out-of-range index or a rating the player has none of.
    [[nodiscard]] float combatRatingBonus(int cr) const;
    /// Health and mana regenerated per second from Spirit, as the stat flyout
    /// shows them. Health splits Spirit at 50 across two tables (gtOCTRegenHP,
    /// gtRegenHPPerSpt) then doubles; mana is Spirit × gtRegenMPPerSpt - the
    /// same as AzerothCore's OCTRegen*PerSpirit.
    [[nodiscard]] float healthRegenFromSpirit() const;
    [[nodiscard]] float manaRegenFromSpirit() const;

    [[nodiscard]] bool helmVisible() const { return sheet_.helmVisible; }
    [[nodiscard]] bool cloakVisible() const { return sheet_.cloakVisible; }
    [[nodiscard]] uint8_t shapeshiftForm() const { return sheet_.shapeshiftForm; }
    /// As the server last confirmed it: 0 stand, 1 sit, 2-6 sitting on
    /// something, 7 dead, 8 kneel.
    [[nodiscard]] uint8_t standState() const { return sheet_.standState; }

    // ---- Home ----
    /// Where the hearthstone goes, once SMSG_BINDPOINTUPDATE has said.
    [[nodiscard]] std::optional<HomeBind> homeBind() const;
    [[nodiscard]] uint32_t homeBindZoneId() const { return sheet_.homeBindZoneId; }
    /// The innkeeper whose "make this your home?" is open, or 0.
    [[nodiscard]] uint64_t binderGuid() const { return sheet_.binderGuid; }
    /// Accept that innkeeper's offer: CMSG_BINDER_ACTIVATE.
    void confirmBinder();

    // ---- Proficiencies (SMSG_SET_PROFICIENCY): one bit per item subclass ----
    [[nodiscard]] uint32_t weaponProficiency() const { return sheet_.weaponProficiency; }
    [[nodiscard]] uint32_t armorProficiency() const { return sheet_.armorProficiency; }

    // ---- Mirror timers ----
    [[nodiscard]] const MirrorTimer& mirrorTimer(int type) const;
    /// Count the running timers on locally. The server sends a timer once with
    /// its value and scale and then speaks only when something changes, so
    /// without this a breath bar stands still.
    void tickMirrorTimers(float dt);

    // ---- Runes ----
    [[nodiscard]] const std::array<RuneSlot, 6>& runes() const { return sheet_.runes; }

    // ---- The barber's chair ----
    [[nodiscard]] bool barberShopOpen() const { return sheet_.barberShopOpen; }
    /// The player got up from the chair.
    void closeBarberShop();

private:
    /// Everything this keeps of the character, assigned whole by
    /// beginCharacter.
    struct Sheet {
        uint32_t xp = 0;
        uint32_t nextLevelXp = 0;
        uint32_t restedXp = 0;
        bool resting = false;
        uint32_t level = 1;
        uint32_t honorPoints = 0;
        uint32_t arenaPoints = 0;
        int32_t armor = 0;
        int32_t resistances[6] = {};  // [0]=Holy,[1]=Fire,[2]=Nature,[3]=Frost,[4]=Shadow,[5]=Arcane
        int32_t stats[5] = {-1, -1, -1, -1, -1};
        int32_t meleeAP = -1;
        int32_t rangedAP = -1;
        int32_t spellDmgBonus[7] = {-1, -1, -1, -1, -1, -1, -1};  // per school 0-6
        int32_t healBonus = -1;
        float manaRegen = 0.0f;         // per second, while not casting
        float manaRegenCasting = 0.0f;  // per second, during the five-second rule
        int32_t expertise = 0;
        int32_t offhandExpertise = 0;
        float dodgePct = -1.0f;
        float parryPct = -1.0f;
        float blockPct = -1.0f;
        float critPct = -1.0f;
        float rangedCritPct = -1.0f;
        float spellCritPct[7] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
        int32_t combatRatings[25] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                     -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        bool helmVisible = true;
        bool cloakVisible = true;
        uint8_t shapeshiftForm = 0;
        uint8_t standState = 0;
        FlatFieldMap fields;
        bool hasHomeBind = false;
        uint32_t homeBindMapId = 0;
        uint32_t homeBindZoneId = 0;
        glm::vec3 homeBindPos{0.0f};  // canonical
        uint64_t binderGuid = 0;
        uint32_t weaponProficiency = 0;
        uint32_t armorProficiency = 0;
        std::array<MirrorTimer, 3> mirrorTimers{};
        std::array<RuneSlot, 6> runes = [] {
            std::array<RuneSlot, 6> r{};
            r[2].type = r[3].type = RuneType::Unholy;
            r[4].type = r[5].type = RuneType::Frost;
            return r;
        }();
        bool barberShopOpen = false;
    };

    void handleLevelUp(network::Packet& packet);
    void handleStandStateUpdate(network::Packet& packet);
    void handlePlayerBound(network::Packet& packet);
    void handleBindPointUpdate(network::Packet& packet);
    void handleStartMirrorTimer(network::Packet& packet);
    void handleStopMirrorTimer(network::Packet& packet);
    void handlePauseMirrorTimer(network::Packet& packet);
    void handleConvertRune(network::Packet& packet);
    void handleResyncRunes(network::Packet& packet);
    void handleAddRunePower(network::Packet& packet);
    void handleBarberShopResult(network::Packet& packet);
    /// Announce a rune's type and readiness to the interface.
    void announceRune(uint32_t index);
    /// "Your home has been set to <zone>."
    void announceHome(const char* lead, uint32_t zoneId);
    /// The level a per-level game table is read at: the player's, the last
    /// row past it.
    [[nodiscard]] uint32_t tableLevel() const;
    [[nodiscard]] float critFromStat(StatTables::Table base, StatTables::Table perStat,
                                     int statIdx) const;

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    PlayerQueries& queries_;
    const StatTables& tables_;
    Sheet sheet_;
};

} // namespace wowee::game
