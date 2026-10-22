#pragma once

#include "core/event_bus.hpp"
#include "game/world_packet_data.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "game/handler_types.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace core { class CVarStore; }
namespace pipeline { class AssetManager; }
namespace game {

class PacketSender;
class FrameXmlEvents;
class ChatLog;
class EntityManager;
class PacketParsers;
struct GameObjectQueryResponseData;
struct CraftRecipe;

/// A skill the character has, as the skill fields carry it.
struct PlayerSkill {
    uint32_t skillId = 0;
    uint16_t value = 0;        // base + permanent item bonuses
    uint16_t maxValue = 0;
    uint16_t bonusTemp = 0;    // temporary buff bonus (food, potions, etc.)
    uint16_t bonusPerm = 0;    // permanent spec/misc bonus (rarely non-zero)
    [[nodiscard]] uint16_t effectiveValue() const { return value + bonusTemp + bonusPerm; }
};

// ---- What Spell.dbc and the tables beside it say ----------------------------

struct SpellReagent { uint32_t itemId = 0; uint32_t count = 0; };

/// One row of Spell.dbc, as much of it as the client reads.
struct SpellNameEntry {
    std::string name; std::string rank; std::string description;
    uint32_t schoolMask = 0; uint8_t dispelType = 0; uint32_t attrEx = 0;
    /// Spell.dbc Attributes, the base word. Bit 6 (0x40) is passive.
    uint32_t attr = 0;
    // Spell.dbc Targets bitmask (SpellCastTargetFlags) - 0x10 = TARGET_FLAG_ITEM
    uint32_t targetFlags = 0;
    // Spell.dbc RangeIndex resolved against SpellRange.dbc. A max range of 0
    // means "Self Only" (shouts, self-buffs); negative means SpellRange.dbc
    // was unavailable, so callers should not infer anything from it.
    float maxRange = -1.0f;
    /// Spell.dbc SpellDescriptionVariableID: which row of
    /// SpellDescriptionVariables.dbc defines the $<name> tokens this spell's
    /// description uses. Zero for the great majority, which use none.
    uint32_t descriptionVariableId = 0;
    int32_t effectBasePoints[3] = {0, 0, 0};
    uint32_t effectIds[3] = {0, 0, 0};
    // Spell.dbc EffectApplyAuraName - which aura an APPLY_AURA effect applies.
    // The effect id above only says that one is applied; this says which, and
    // it is the only thing that distinguishes a tracking spell (44 creatures,
    // 45 resources) from any other buff.
    uint32_t effectAuraIds[3] = {0, 0, 0};
    // Spell.dbc EffectImplicitTargetA - what the spell expects to be aimed at.
    // 21 means a friendly unit, which is how heals and buffs are told apart
    // from damage that shares the same effect and school.
    uint32_t implicitTargetA = 0;
    float durationSec = 0.0f;
    uint32_t spellVisualId = 0;
    uint32_t recoveryMs = 0;
    uint32_t categoryRecoveryMs = 0;
    uint32_t createdItemId = 0;
    SpellReagent reagents[8] = {};
    uint32_t trivialSkillHigh = 0;
    uint32_t trivialSkillLow = 0;
    uint32_t minSkillRank = 0;
    // Spell.dbc TargetAuraState: the aura state the target must be in for the
    // spell to be castable (e.g. Execute -> AURA_STATE_HEALTHLESS_20_PERCENT).
    // 0 = none.
    uint32_t targetAuraState = 0;
};

/// A spell's cast time, range and cost, as the client's spell data resolves
/// them.
struct SpellDataInfo {
    uint32_t castTimeMs = 0;
    float minRange = 0;
    float maxRange = 0;
    uint32_t manaCost = 0;
    uint8_t powerType = 0;
};

// ---- Talent modifiers (SMSG_SET_FLAT_SPELL_MODIFIER / PCT) -------------------

enum class SpellModOp : uint8_t {
    Damage            =  0,
    Duration          =  1,
    Threat            =  2,
    Effect1           =  3,
    Charges           =  4,
    Range             =  5,
    Radius            =  6,
    CritChance        =  7,
    AllEffects        =  8,
    NotLoseCastingTime =  9,
    CastingTime       = 10,
    Cooldown          = 11,
    Effect2           = 12,
    IgnoreArmor       = 13,
    Cost              = 14,
    CritDamageBonus   = 15,
    ResistMissChance  = 16,
    JumpTargets       = 17,
    ChanceOfSuccess   = 18,
    ActivationTime    = 19,
    // From here the list was the modern one - Efficiency, MultipleValue,
    // ResistPushback and the rest are Cataclysm names - grafted onto a 3.3.5
    // head. The numbers are what the server sends: a talent that modifies a
    // periodic effect arrives as op 22, and a table calling that
    // ResistDispelChance would have applied it to dispel resistance.
    DamageMultiplier  = 20,
    GlobalCooldown    = 21,
    Dot               = 22,
    Effect3           = 23,
    BonusMultiplier   = 24,
    // 25 is not used by this client version.
    ProcPerMinute     = 26,
    ValueMultiplier   = 27,
    ResistDispelChance = 28,
    CritDamageBonus2  = 29,
    SpellCostRefundOnFail = 30,
};
inline constexpr int kSpellModOpCount = 32;

// Key: (SpellModOp, groupIndex) - value: accumulated flat or pct modifier.
// pct values are stored in integer percent (e.g. -20 means -20% reduction).
struct SpellModKey {
    SpellModOp op;
    uint8_t    group;
    bool operator==(const SpellModKey& o) const {
        return op == o.op && group == o.group;
    }
};
struct SpellModKeyHash {
    std::size_t operator()(const SpellModKey& k) const {
        return std::hash<uint32_t>()(
            (static_cast<uint32_t>(static_cast<uint8_t>(k.op)) << 8) | k.group);
    }
};
using SpellModMap = std::unordered_map<SpellModKey, int32_t, SpellModKeyHash>;

// ---- Totems ---------------------------------------------------------------------

struct TotemSlot {
    uint32_t spellId     = 0;
    uint32_t durationMs  = 0;
    std::chrono::steady_clock::time_point placedAt{};
    [[nodiscard]] bool active() const { return spellId != 0 && remainingMs() > 0; }
    [[nodiscard]] float remainingMs() const {
        if (spellId == 0 || durationMs == 0) return 0.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - placedAt).count();
        float rem = static_cast<float>(durationMs) - static_cast<float>(elapsed);
        return rem > 0.0f ? rem : 0.0f;
    }
};
inline constexpr int kTotemSlots = 4;

/// PLAYER_EXPLORED_ZONES_1 and the words after it: one bit per explored area.
inline constexpr size_t kExploredZoneMaskCount = 128;

/// Where the hearthstone goes.
struct HomeBind {
    uint32_t mapId = 0;
    glm::vec3 position{0.0f};  ///< canonical
};

/// What the spell book asks of the rest of the game, and the few things a
/// cast has the rest of the game do.
///
/// Each answer belongs to another domain - who is selected, where the player
/// stands and faces, the mount, the fight, the items, the game object being
/// opened - and is given by whoever owns it. Grouped by owner, so the parts
/// can be handed over separately as those owners stop being GameHandler.
class SpellQueries {
public:
    virtual ~SpellQueries() = default;

    // ---- The player and the world in view ----
    [[nodiscard]] virtual uint64_t characterGuid() const = 0;
    [[nodiscard]] virtual uint64_t currentTargetGuid() const = 0;
    [[nodiscard]] virtual uint64_t focusTargetGuid() const = 0;
    [[nodiscard]] virtual uint64_t petGuid() const = 0;
    [[nodiscard]] virtual const EntityManager& entities() const = 0;
    [[nodiscard]] virtual PacketParsers& packetParsers() = 0;
    /// "player", "target", "party2", ... - the unit id FrameXML knows a guid
    /// by, or empty.
    [[nodiscard]] virtual std::string unitIdFor(uint64_t guid) const = 0;
    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    [[nodiscard]] virtual uint32_t playerLevel() const = 0;
    [[nodiscard]] virtual const GameObjectQueryResponseData* gameObjectInfo(uint32_t entry) const = 0;
    [[nodiscard]] virtual bool friendlyFaction(uint32_t factionTemplateId) const = 0;
    [[nodiscard]] virtual bool aggressiveTowardPlayer(uint64_t guid) const = 0;

    // ---- Where the player stands and faces ----
    [[nodiscard]] virtual const MovementInfo& playerMovement() const = 0;
    virtual void sendMovement(LogicalOpcode opcode) = 0;
    /// Set the character's facing (canonical yaw) and tell the server.
    virtual void setFacing(float canonicalYaw) = 0;
    /// Turn the character to face a direction, as a target is faced.
    virtual void faceTowards(float canonicalYaw) = 0;
    /// The direction the camera looks, the character turned to it.
    virtual float faceCamera() = 0;
    [[nodiscard]] virtual bool autoFaceTarget() const = 0;
    [[nodiscard]] virtual bool autoSelfCast() const = 0;
    [[nodiscard]] virtual bool flying() const = 0;
    virtual void setStandState(uint8_t standState) = 0;

    // ---- The mount ----
    [[nodiscard]] virtual bool mounted() const = 0;
    [[nodiscard]] virtual uint32_t mountDisplayId() const = 0;
    [[nodiscard]] virtual uint32_t mountAuraSpellId() const = 0;
    /// The aura keeping the player mounted turned out to be this one.
    virtual void noteMountAura(uint32_t spellId) = 0;
    virtual void dismount() = 0;

    // ---- The fight ----
    [[nodiscard]] virtual bool autoAttacking() const = 0;
    [[nodiscard]] virtual bool inCombat() const = 0;
    virtual void startAutoAttack(uint64_t targetGuid) = 0;
    virtual void stopAutoAttack() = 0;
    virtual void addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId,
                               bool isPlayerSource, uint8_t powerType,
                               uint64_t srcGuid, uint64_t dstGuid) = 0;
    /// A spellsteal is logged once though the server reports it twice.
    virtual bool shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid,
                                         uint32_t spellId) = 0;
    /// The next melee swing animation is this ability's, not an auto attack's.
    virtual void suppressNextMeleeSwingAnim() = 0;

    // ---- Items ----
    [[nodiscard]] virtual const ItemQueryResponseData* itemInfo(uint32_t itemId) const = 0;
    [[nodiscard]] virtual const std::unordered_map<uint32_t, ItemQueryResponseData>&
    itemTemplates() const = 0;
    virtual void ensureItemInfo(uint32_t itemId) = 0;
    [[nodiscard]] virtual uint32_t itemEntryByGuid(uint64_t itemGuid) const = 0;
    [[nodiscard]] virtual uint32_t itemCountInBags(uint32_t itemId) const = 0;
    [[nodiscard]] virtual bool awaitingItemTarget() const = 0;
    virtual void beginSpellItemTargeting(uint32_t spellId, const std::string& spellName) = 0;

    // ---- The game object being opened or gathered ----
    [[nodiscard]] virtual uint64_t pendingGameObjectInteraction() const = 0;
    [[nodiscard]] virtual uint64_t lastInteractedGameObject() const = 0;
    virtual void clearPendingGameObjectInteraction() = 0;
    virtual void clearLastInteractedGameObject() = 0;
    virtual void despawnGameObjectLocally(uint64_t guid) = 0;
    virtual void lootTarget(uint64_t guid) = 0;

    // ---- Companions and the hearth ----
    [[nodiscard]] virtual uint32_t activeCritterSpellId() const = 0;
    [[nodiscard]] virtual uint64_t activeCritterGuid() const = 0;
    virtual void dismissCritter() = 0;
    /// A mount or critter may have come out or gone away.
    virtual void announceCompanionChange() = 0;
    [[nodiscard]] virtual std::optional<HomeBind> homeBind() const = 0;

    // ---- Achievements ----
    [[nodiscard]] virtual std::string achievementName(uint32_t achievementId) = 0;
    virtual void recordAchievementEarned(uint32_t achievementId, uint32_t packedDate) = 0;

    // ---- What the client's own data resolves ----
    [[nodiscard]] virtual SpellDataInfo spellData(uint32_t spellId) const = 0;
    [[nodiscard]] virtual std::string iconPath(uint32_t iconId) const = 0;
    virtual void addLocalChatMessage(const MessageChatData& message) = 0;
};

class SpellHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    SpellHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                 ChatLog& chat, SpellQueries& queries, const core::CVarStore& cvars,
                 pipeline::AssetManager* assets);

    void registerOpcodes(DispatchTable& table);

    // Talent data structures (aliased from handler_types.hpp)
    using TalentEntry = game::TalentEntry;
    using TalentTabEntry = game::TalentTabEntry;

    // --- Spell book tabs ---
    struct SpellBookTab {
        std::string name;
        std::string texture; // icon path
        std::vector<uint32_t> spellIds; // all visible learned ranks in this tab
        // One-based slots in the full book, for the compact highest-rank view.
        std::vector<uint32_t> highestRankSlots;
    };

    // Unit cast state (aliased from handler_types.hpp)
    using UnitCastState = game::UnitCastState;

    // Equipment set info (aliased from handler_types.hpp)
    using EquipmentSetInfo = game::EquipmentSetInfo;

    // --- Public API (delegated from GameHandler) ---
    void castSpell(uint32_t spellId, uint64_t targetGuid = 0);

    /// Spell.dbc EffectImplicitTargetA, or 0 when the spell is unknown. 21 means
    /// the spell has to be aimed at a friendly unit.
    [[nodiscard]] uint32_t getSpellImplicitTargetA(uint32_t spellId) const;
    /// Whether Spell.dbc has this spell at all.
    ///
    /// A server may cast something the client's own data has never heard of -
    /// a private core's custom item does it routinely - and the answer to
    /// "what does this aim at" is then not zero but unknown. The two are worth
    /// telling apart: zero means the spell says nothing, unknown means we do.
    [[nodiscard]] bool isSpellKnownToClient(uint32_t spellId) const;

    /// The last spell the player cast while on foot. When mounting is detected,
    /// this identifies which of the player's indefinite self-cast auras is the
    /// mount - scanning for one blindly can land on a racial or a tracking buff.
    [[nodiscard]] uint32_t getLastGroundCastSpellId() const { return lastGroundCastSpellId_; }

    /// Record a spell cast by using an item - pre-WotLK mounts are items, and
    /// their on-use spell never passes through castSpell().
    void noteGroundCastSpell(uint32_t spellId) { lastGroundCastSpellId_ = spellId; }
    void cancelCast();
    void cancelAura(uint32_t spellId);

    /// Whether the player carries an aura of this spell.
    [[nodiscard]] bool carriesAura(uint32_t spellId) const;

    // Known spells
    [[nodiscard]] const std::unordered_set<uint32_t>& getKnownSpells() const { return knownSpells_; }
    [[nodiscard]] const std::unordered_map<uint32_t, float>& getSpellCooldowns() const { return spellCooldowns_; }
    [[nodiscard]] float getSpellCooldown(uint32_t spellId) const;
    [[nodiscard]] float getSpellCooldownTotal(uint32_t spellId) const;

    // Cast state
    [[nodiscard]] bool isCasting() const { return casting_ || restorationActive_; }
    [[nodiscard]] bool isChanneling() const { return casting_ ? castIsChannel_ : restorationActive_; }
    [[nodiscard]] bool isRestoring() const { return restorationActive_; }
    /// Eating rather than drinking, while isRestoring().
    [[nodiscard]] bool isRestorationFood() const { return restorationIsFood_; }
    [[nodiscard]] bool isGameObjectInteractionCasting() const;
    [[nodiscard]] uint32_t getCurrentCastSpellId() const {
        return casting_ ? currentCastSpellId_ : restorationSpellId_;
    }
    [[nodiscard]] float getCastProgress() const {
        const float total = casting_ ? castTimeTotal_ : restorationTimeTotal_;
        const float remaining = casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
        return total > 0.0f ? (total - remaining) / total : 0.0f;
    }
    [[nodiscard]] float getCastTimeRemaining() const {
        return casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
    }
    [[nodiscard]] float getCastTimeTotal() const {
        return casting_ ? castTimeTotal_ : restorationTimeTotal_;
    }

    // Repeat-craft queue
    void startCraftQueue(uint32_t spellId, int count);
    void cancelCraftQueue();
    [[nodiscard]] int getCraftQueueRemaining() const { return craftQueueRemaining_; }
    [[nodiscard]] uint32_t getCraftQueueSpellId() const { return craftQueueSpellId_; }

    // Crafting window (client-side; opened by casting a profession spell
    // like Cooking or First Aid - see tradeskillOpenerSkillLine)
    [[nodiscard]] bool isCraftingWindowOpen() const { return craftingWindowOpen_; }
    [[nodiscard]] uint32_t getCraftingSkillLine() const { return craftingSkillLine_; }
    /// Opening and closing a profession announce themselves, because the
    /// interface's trade skill panel is driven entirely by these two events -
    /// it hides on TRADE_SKILL_CLOSE and fills itself on TRADE_SKILL_SHOW.
    /// Without them the panel could be complete and still never appear.
    void openCraftingWindow(uint32_t skillLine);
    void closeCraftingWindow();
    // Returns the skill line id if spellId is a tradeskill-window opener
    // (e.g. Cooking → 185) with at least one known recipe, else 0.
    uint32_t tradeskillOpenerSkillLine(uint32_t spellId);

    // SpellFocusObject.dbc name ("Anvil", "Cooking Fire", ...) for
    // requires-spell-focus cast failures; empty if unknown.
    const std::string& getSpellFocusName(uint32_t focusId);

    // TotemCategory.dbc name ("Blacksmith Hammer", "Mining Pick", ...) for
    // totem-category cast failures; empty if unknown.
    const std::string& getTotemCategoryName(uint32_t categoryId);

    // Spell queue (400ms window)
    [[nodiscard]] uint32_t getQueuedSpellId() const { return queuedSpellId_; }
    void cancelQueuedSpell() { queuedSpellId_ = 0; queuedSpellTarget_ = 0; }

    // Unit cast state (tracked per GUID for target frame + boss frames)
    [[nodiscard]] const UnitCastState* getUnitCastState(uint64_t guid) const {
        auto it = unitCastStates_.find(guid);
        return (it != unitCastStates_.end() && it->second.casting) ? &it->second : nullptr;
    }
    void clearUnitCastStates() { unitCastStates_.clear(); }
    void removeUnitCastState(uint64_t guid) { unitCastStates_.erase(guid); }

    // Aura cache mutation (formerly accessed via friend)
    void clearUnitAurasCache() { unitAurasCache_.clear(); }
    /// Copy a unit's auras into the by-guid list the party frames read.
    void mirrorAurasByGuid(uint64_t guid, const std::vector<AuraSlot>& auras);
    void removeUnitAuraCache(uint64_t guid) { unitAurasCache_.erase(guid); }

    // Known spells mutation (formerly accessed via friend)
    void addKnownSpell(uint32_t spellId) {
        if (knownSpells_.insert(spellId).second) spellBookTabsDirty_ = true;
    }
    [[nodiscard]] bool hasKnownSpell(uint32_t spellId) const { return knownSpells_.count(spellId) > 0; }

    // Target aura mutation (formerly accessed via friend)
    void clearTargetAuras() { for (auto& slot : targetAuras_) slot = AuraSlot{}; }

    // Player aura mutation (formerly accessed via friend)
    void resetPlayerAuras(size_t capacity) { playerAuras_.clear(); playerAuras_.resize(capacity); }
    AuraSlot& getPlayerAuraSlotRef(size_t slot) { return playerAuras_[slot]; }
    std::vector<AuraSlot>& getPlayerAurasMut() { return playerAuras_; }

    // Target cast helpers
    [[nodiscard]] bool isTargetCasting() const;
    [[nodiscard]] uint32_t getTargetCastSpellId() const;
    [[nodiscard]] float getTargetCastProgress() const;
    [[nodiscard]] float getTargetCastTimeRemaining() const;
    [[nodiscard]] bool isTargetCastInterruptible() const;

    // Talents
    [[nodiscard]] uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    [[nodiscard]] uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    [[nodiscard]] uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        static std::unordered_map<uint32_t, uint8_t> empty;
        return spec < 2 ? learnedTalents_[spec] : empty;
    }

    static constexpr uint8_t MAX_GLYPH_SLOTS = 6;
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs() const { return learnedGlyphs_[activeTalentSpec_]; }
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs(uint8_t spec) const {
        static std::array<uint16_t, MAX_GLYPH_SLOTS> empty{};
        return spec < 2 ? learnedGlyphs_[spec] : empty;
    }
    [[nodiscard]] uint8_t getTalentRank(uint32_t talentId) const {
        auto it = learnedTalents_[activeTalentSpec_].find(talentId);
        return (it != learnedTalents_[activeTalentSpec_].end()) ? it->second : 0;
    }
    void learnTalent(uint32_t talentId, uint32_t requestedRank);
    void switchTalentSpec(uint8_t newSpec);

    // Talent DBC access
    [[nodiscard]] const TalentEntry* getTalentEntry(uint32_t talentId) const {
        auto it = talentCache_.find(talentId);
        return (it != talentCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const TalentTabEntry* getTalentTabEntry(uint32_t tabId) const {
        auto it = talentTabCache_.find(tabId);
        return (it != talentTabCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentEntry>& getAllTalents() const { return talentCache_; }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentTabEntry>& getAllTalentTabs() const { return talentTabCache_; }

    /// Drops what was read out of Talent.dbc and TalentTab.dbc.
    ///
    /// Called when the active expansion changes, for the same reason as
    /// MovementHandler::resetTaxiDbcCache: GameHandler cleared copies of its
    /// own and the readers come here.
    void resetTalentDbcCache() {
        talentCache_.clear();
        talentTabCache_.clear();
        talentDbcLoaded_ = false;
    }
    void loadTalentDbc();
    void syncPreWotlkTalentsFromKnownSpells();

    // Auras
    [[nodiscard]] const std::vector<AuraSlot>& getPlayerAuras() const { return playerAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>& getTargetAuras() const { return targetAuras_; }
    /// Every unit's auras but the player's, by guid - the target's included.
    [[nodiscard]] const std::unordered_map<uint64_t, std::vector<AuraSlot>>& getUnitAurasByGuid() const {
        return unitAurasCache_;
    }
    [[nodiscard]] const std::vector<AuraSlot>* getUnitAuras(uint64_t guid) const {
        auto it = unitAurasCache_.find(guid);
        return (it != unitAurasCache_.end()) ? &it->second : nullptr;
    }

    // Global Cooldown (GCD)
    [[nodiscard]] float getGCDRemaining() const {
        if (gcdTotal_ <= 0.0f) return 0.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gcdStartedAt_).count() / 1000.0f;
        float rem = gcdTotal_ - elapsed;
        return rem > 0.0f ? rem : 0.0f;
    }
    [[nodiscard]] float getGCDTotal() const { return gcdTotal_; }
    [[nodiscard]] bool isGCDActive() const { return getGCDRemaining() > 0.0f; }

    // Spell book tabs
    const std::vector<SpellBookTab>& getSpellBookTabs();
    uint32_t getKnownSlotFromHighestRankSlot(uint32_t slot);

    // Talent wipe confirm dialog
    [[nodiscard]] bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    [[nodiscard]] uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    /// The trainer offering the wipe. The confirmation closes itself when the
    /// player walks away from them.
    [[nodiscard]] uint64_t getTalentWipeNpcGuid() const { return talentWipeNpcGuid_; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }

    // Spell DBC accessors
    [[nodiscard]] const int32_t* getSpellEffectBasePoints(uint32_t spellId) const;
    [[nodiscard]] float getSpellDuration(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellName(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellRank(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellDescription(uint32_t spellId) const;
    [[nodiscard]] std::string getEnchantName(uint32_t enchantId) const;
    /// The gem item an enchantment came out of (SpellItemEnchantment.Src_ItemID).
    /// Zero when the enchantment is not a gem, or on a file with no such column.
    /// This is the only route from an enchantment sitting in an item's socket
    /// back to the gem that is in the socket - the item fields carry the
    /// enchantment id and nothing else.
    [[nodiscard]] uint32_t getEnchantGemItem(uint32_t enchantId) const;
    [[nodiscard]] uint8_t getSpellDispelType(uint32_t spellId) const;
    [[nodiscard]] bool isSpellInterruptible(uint32_t spellId) const;
    [[nodiscard]] bool isSpellPassive(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellSchoolMask(uint32_t spellId) const;
    /// Spell.dbc SpellVisualID - the effect drawn for the spell - or 0.
    [[nodiscard]] uint32_t getSpellVisualId(uint32_t spellId) const;
    /// Spell.dbc Targets mask (SpellCastTargetFlags): 0x10 = TARGET_FLAG_ITEM.
    [[nodiscard]] uint32_t getSpellTargetFlags(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellTargetAuraState(uint32_t spellId) const;
    /// Spell.dbc RangeIndex resolved via SpellRange.dbc, in yards. Melee ("Combat
    /// Range") is 5; self-only is 0; negative means SpellRange.dbc was unavailable.
    [[nodiscard]] float getSpellMaxRange(uint32_t spellId) const;
    /// True for "Self Only" range spells (shouts, self-buffs): they always land on
    /// the caster, so they take no explicit target and skip melee range checks.
    [[nodiscard]] bool isSelfCastSpell(uint32_t spellId) const;
    /// Maps a superseded spell rank to the highest rank we actually know. Returns
    /// spellId unchanged when it is already known, or has no known same-name rank.
    [[nodiscard]] uint32_t resolveHighestKnownRank(uint32_t spellId) const;
    /// The skill's own name, by SkillLine.dbc id.
    [[nodiscard]] const std::string& getSkillLineName(uint32_t skillLineId) const;

    /// A row of Spell.dbc, or null for a spell this install has never heard
    /// of. Reads the file on the first ask.
    [[nodiscard]] const SpellNameEntry* findSpell(uint32_t spellId) const;
    /// SpellDescriptionVariables.dbc: the $<name> declarations by row.
    [[nodiscard]] const std::string* spellDescriptionVariables(uint32_t variableId) const;
    /// Drops what was read out of Spell.dbc, SkillLine.dbc and
    /// SkillLineAbility.dbc, for an expansion switch.
    void resetSpellDbcCaches();

    // ---- Skills ----
    [[nodiscard]] const std::unordered_map<uint32_t, PlayerSkill>& playerSkills() const {
        return character_.skills;
    }
    /// SkillLine.dbc's category for a skill: 7 class, 9 secondary, 11 profession...
    [[nodiscard]] uint32_t skillCategory(uint32_t skillLineId) const;
    [[nodiscard]] const std::string& skillCategoryName(uint32_t categoryId) const;
    /// Where a category's heading sorts; unknown ones sort last.
    [[nodiscard]] uint32_t skillCategorySortIndex(uint32_t categoryId) const;
    [[nodiscard]] const std::string& skillLineDescription(uint32_t skillLineId) const;
    /// The skill line a spell teaches, by SkillLineAbility.dbc; 0 for none.
    [[nodiscard]] uint32_t skillLineOfSpell(uint32_t spellId) const;
    /// A primary or secondary profession's spell (category 11 or 9).
    [[nodiscard]] bool isProfessionSpell(uint32_t spellId) const;
    /// Where a recipe sits against the player's skill, 0 to 3, in the bands
    /// the crafting window colours by.
    [[nodiscard]] int recipeDifficulty(uint32_t spellId) const;
    /// The recipes of the open crafting window's profession that the player
    /// knows, with how many each can make from what is in the bags.
    [[nodiscard]] std::vector<CraftRecipe> craftingRecipes() const;

    // ---- Talent modifiers ----
    [[nodiscard]] const SpellModMap& spellFlatMods() const { return character_.flatMods; }
    [[nodiscard]] const SpellModMap& spellPctMods() const { return character_.pctMods; }
    /// The sum of every flat (or percent) modifier for an op, across groups.
    [[nodiscard]] int32_t spellFlatMod(SpellModOp op) const;
    [[nodiscard]] int32_t spellPctMod(SpellModOp op) const;

    // ---- Explored zones ----
    [[nodiscard]] const std::vector<uint32_t>& exploredZoneMasks() const {
        return character_.exploredZones;
    }
    [[nodiscard]] bool hasExploredZoneMasks() const { return character_.hasExploredZones; }

    // ---- Totems ----
    [[nodiscard]] const TotemSlot& totemSlot(int slot) const;

    // Cast state
    void stopCasting();
    void resetCastState();
    void resetTalentState();
    // Full per-character reset (spells, cooldowns, auras, cast state, talents).
    // Called from GameHandler::selectCharacter so spell state doesn't bleed between characters.
    void resetAllState();
    void clearUnitCaches();

    // Aura duration
    void handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs);

    // Skill DBC
    void loadSkillLineDbc() const;
    void extractSkillFields(const FlatFieldMap& fields);
    void extractExploredZoneFields(const FlatFieldMap& fields);

    // Update per-frame timers (call from GameHandler::update)
    void updateTimers(float dt);
    void refreshRestorationState() { refreshRestorationFromPlayerAuras(); }

    // DBC cache loading (called from GameHandler during login)
    void loadSpellNameCache() const;
    void loadSkillLineAbilityDbc() const;

    /// The five values every UNIT_SPELLCAST_* event carries.
    ///
    /// unit, spell name, rank, cast id, spell id - in that order, which is what
    /// FrameXML unpacks. These were being fired as just the unit and the spell
    /// id, so the id sat where the name belongs and the cast id was absent.
    [[nodiscard]] std::vector<std::string> spellcastArgs(const std::string& unitId,
                                           uint32_t spellId) const;

private:
    // --- Packet handlers ---
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    void handleSpellCooldown(network::Packet& packet);
    void handleCooldownEvent(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleLearnedSpell(network::Packet& packet);

    void handleCastResult(network::Packet& packet);
    void handleSpellFailedOther(network::Packet& packet);
    void handleClearCooldown(network::Packet& packet);
    void handleModifyCooldown(network::Packet& packet);
    void handlePlaySpellVisual(network::Packet& packet);
    void handleSpellModifier(network::Packet& packet, bool isFlat);
    void handleSpellDelayed(network::Packet& packet);
    void handleSpellLogMiss(network::Packet& packet);
    void handleSpellFailure(network::Packet& packet);
    void handleItemCooldown(network::Packet& packet);
    void handleDispelFailed(network::Packet& packet);
    void handleTotemCreated(network::Packet& packet);
    void handlePeriodicAuraLog(network::Packet& packet);
    void handleSpellEnergizeLog(network::Packet& packet);
    void handleExtraAuraInfo(network::Packet& packet, bool isInit);
    void handleSpellDispelLog(network::Packet& packet);
    void handleSpellStealLog(network::Packet& packet);
    void handleSpellChanceProcLog(network::Packet& packet);
    void handleSpellInstaKillLog(network::Packet& packet);
    void handleSpellLogExecute(network::Packet& packet);
    void handleClearExtraAuraInfo(network::Packet& packet);
    void handleResumeCastBar(network::Packet& packet);
    void handleChannelStart(network::Packet& packet);
    void handleChannelUpdate(network::Packet& packet);

    // --- Internal helpers ---

    void refreshRestorationFromPlayerAuras();
    void stopRestorationPresentation();

    // --- handleSpellLogExecute per-effect parsers (extracted to reduce nesting) ---
    void parseEffectPowerDrain(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                               bool usesFullGuid);
    void parseEffectHealthLeech(network::Packet& packet, uint32_t effectLogCount,
                                uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                                bool usesFullGuid);
    void parseEffectCreateItem(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectInterruptCast(network::Packet& packet, uint32_t effectLogCount,
                                  uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                                  bool usesFullGuid);
    void parseEffectFeedPet(network::Packet& packet, uint32_t effectLogCount,
                            uint64_t caster, uint32_t spellId, bool isPlayerCaster);

    // Find the on-use spell for an item (trigger=0 Use or trigger=5 NoDelay).
    // CMSG_USE_ITEM requires a valid spellId or the server silently ignores it.
    void seedCooldownFromSpellInfo(uint32_t spellId);
    void handleSupercededSpell(network::Packet& packet);
    void handleRemovedSpell(network::Packet& packet);
    void handleUnlearnSpells(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);
    void handleAchievementEarned(network::Packet& packet);

    // Wording for a refused cast, from the packet's codes and what the
    // client's data knows about the spell, the tool or the node.
    [[nodiscard]] bool isBandageSpell(uint32_t spellId) const;
    [[nodiscard]] std::string castFailureMessage(uint32_t spellId, uint8_t result, int powerType,
                                                 uint32_t miscArg = 0, uint32_t miscArg2 = 0);
    [[nodiscard]] uint32_t gatherRequiredSkillRank(uint64_t goGuid) const;
    [[nodiscard]] std::string gatherCastFailureMessage(uint64_t goGuid, uint32_t spellId,
                                                       uint8_t result, const std::string& fallback) const;
    [[nodiscard]] std::string displaySpellName(uint32_t spellId) const;
    [[nodiscard]] std::string formatSpellNameList(const std::vector<uint32_t>& spellIds,
                                                  size_t maxShown = 3) const;
    void raiseUiError(const std::string& message);
    /// A unit started or stopped a cast or channel: the client animates it
    /// (events::CastAnimationChanged).
    void announceCastAnimation(uint64_t casterGuid, bool started, bool channel,
                               SpellCastType castType);

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    SpellQueries& queries_;
    const core::CVarStore& cvars_;
    /// The client's data, for the DBC tables. Null in a test that has none,
    /// which leaves every spell unknown to the client.
    pipeline::AssetManager* const assets_;

    // --- What the client's data says (read once, kept until an expansion switch) ---
    mutable std::unordered_map<uint32_t, SpellNameEntry> spellNameCache_;
    mutable bool spellNameCacheLoaded_ = false;
    mutable std::unordered_map<uint32_t, std::string> spellDescriptionVariables_;
    mutable std::unordered_map<uint32_t, std::string> skillLineNames_;
    mutable std::unordered_map<uint32_t, uint32_t> skillLineCategories_;
    mutable std::unordered_map<uint32_t, std::string> skillCategoryNames_;
    mutable std::unordered_map<uint32_t, uint32_t> skillCategorySort_;
    mutable std::unordered_map<uint32_t, uint32_t> skillLineIcons_;
    mutable std::unordered_map<uint32_t, std::string> skillLineDescriptions_;
    mutable std::unordered_map<uint32_t, uint32_t> spellToSkillLine_;   // spellID -> skillLineID
    mutable bool skillLineDbcLoaded_ = false;
    mutable bool skillLineAbilityLoaded_ = false;

    /// The character's skills, talent modifiers, explored map and totems, as
    /// the realm has described them. Assigned whole by resetAllState, so a
    /// field added here cannot keep the last character's value.
    struct CharacterSpellState {
        std::unordered_map<uint32_t, PlayerSkill> skills;
        SpellModMap flatMods;
        SpellModMap pctMods;
        std::vector<uint32_t> exploredZones = std::vector<uint32_t>(kExploredZoneMaskCount, 0u);
        bool hasExploredZones = false;
        std::array<TotemSlot, kTotemSlots> totems{};
    };
    CharacterSpellState character_;

    // --- Spell state ---
    std::unordered_set<uint32_t> knownSpells_;
    std::unordered_map<uint32_t, float> spellCooldowns_;    // spellId -> remaining seconds
    // spellId -> the length the cooldown had when it began. Kept beside the
    // remaining time rather than derived from it because GetSpellCooldown is
    // asked for (start, duration), and answering (now, remaining) redraws the
    // swirl as a fresh full sweep every time the interface asks - which it does
    // on every ACTIONBAR_UPDATE_COOLDOWN, so a long cooldown appears to restart
    // whenever anything else is cast.
    std::unordered_map<uint32_t, float> spellCooldownTotals_;
    uint8_t castCount_ = 0;
    bool casting_ = false;
    bool castIsChannel_ = false;
    uint32_t currentCastSpellId_ = 0;
    float castTimeRemaining_ = 0.0f;
    float castTimeTotal_ = 0.0f;
    bool restorationActive_ = false;
    uint32_t restorationSpellId_ = 0;
    bool restorationIsFood_ = false;
    float restorationTimeRemaining_ = 0.0f;
    float restorationTimeTotal_ = 0.0f;

    // Repeat-craft queue
    uint32_t craftQueueSpellId_ = 0;
    int craftQueueRemaining_ = 0;

    // Crafting window
    bool craftingWindowOpen_ = false;
    uint32_t craftingSkillLine_ = 0;

    // SpellFocusObject.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> spellFocusNames_;
    bool spellFocusDbcLoaded_ = false;

    // TotemCategory.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> totemCategoryNames_;
    bool totemCategoryDbcLoaded_ = false;

    // Spell queue (400ms window)
    uint32_t lastGroundCastSpellId_ = 0;
    /// When auto-attack was last toggled, for the Ability Toggle guard: a
    /// second press inside a short window is an accident rather than a
    /// decision. See the SPELL_ID_ATTACK branch in castSpell.
    std::chrono::steady_clock::time_point autoAttackToggledAt_{};
    uint32_t queuedSpellId_ = 0;
    uint64_t queuedSpellTarget_ = 0;

    // Per-unit cast state
    std::unordered_map<uint64_t, UnitCastState> unitCastStates_;

    // Talents (dual-spec support)
    uint8_t activeTalentSpec_ = 0;
    uint8_t unspentTalentPoints_[2] = {0, 0};
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];
    std::array<std::array<uint16_t, MAX_GLYPH_SLOTS>, 2> learnedGlyphs_{};
    std::unordered_map<uint32_t, TalentEntry> talentCache_;
    std::unordered_map<uint32_t, TalentTabEntry> talentTabCache_;
    bool talentDbcLoaded_ = false;
    bool talentsInitialized_ = false;

    // Auras
    std::vector<AuraSlot> playerAuras_;
    std::vector<AuraSlot> targetAuras_;
    std::unordered_map<uint64_t, std::vector<AuraSlot>> unitAurasCache_;

    // Global Cooldown
    float gcdTotal_ = 0.0f;
    std::chrono::steady_clock::time_point gcdStartedAt_{};

    // Spell book tabs
    std::vector<SpellBookTab> spellBookTabs_;
    size_t lastSpellCount_ = 0;
    bool spellBookTabsDirty_ = true;
    std::unordered_set<uint32_t> warnedSpellBookIcons_;

    // Talent wipe confirm dialog
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;

    /// Last, so it goes first: its handlers write the spell book.
    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace game
} // namespace wowee
