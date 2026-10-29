#pragma once

#include "game/world_packet_data.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "network/packet.hpp"
#include <glm/vec3.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace core { class EventBus; class CVarStore; }
namespace game {

class ChatLog;
class FrameXmlEvents;
class GameHandler;
class PacketSender;
class Entity;

class CombatHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    CombatHandler(GameHandler& owner, PacketSender& sender, FrameXmlEvents& frameXml,
                  core::EventBus& events, ChatLog& chat, const core::CVarStore& cvars);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---
    void startAutoAttack(uint64_t targetGuid);
    void stopAutoAttack();
    [[nodiscard]] bool isAutoAttacking() const { return autoAttacking_; }
    [[nodiscard]] bool hasAutoAttackIntent() const { return autoAttackRequested_; }
    [[nodiscard]] bool isInCombat() const { return autoAttacking_ || !hostileAttackers_.empty(); }
    [[nodiscard]] bool isInCombatWith(uint64_t guid) const {
        return guid != 0 &&
               ((autoAttacking_ && autoAttackTarget_ == guid) ||
                (hostileAttackers_.count(guid) > 0));
    }
    [[nodiscard]] uint64_t getAutoAttackTargetGuid() const { return autoAttackTarget_; }
    [[nodiscard]] bool isAggressiveTowardPlayer(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    [[nodiscard]] uint64_t getLastMeleeSwingMs() const { return lastMeleeSwingMs_; }

    /// The rogue's or druid's combo points, as SMSG_UPDATE_COMBO_POINTS last
    /// said.
    [[nodiscard]] uint8_t comboPoints() const { return comboPoints_; }
    /// What is in view changed, so Tab builds its list of targets afresh.
    void invalidateTabCycle() { tabCycleStale_ = true; }

    // Floating combat text
    [[nodiscard]] const std::vector<CombatTextEntry>& getCombatText() const { return combatText_; }
    void updateCombatText(float deltaTime);

    // Combat log (persistent rolling history)
    [[nodiscard]] const std::deque<CombatLogEntry>& getCombatLog() const { return combatLog_; }
    void clearCombatLog() { combatLog_.clear(); }

    // Threat
    struct ThreatEntry {
        uint64_t victimGuid = 0;
        uint32_t threat     = 0;
    };
    [[nodiscard]] const std::vector<ThreatEntry>* getThreatList(uint64_t unitGuid) const {
        auto it = threatLists_.find(unitGuid);
        return (it != threatLists_.end()) ? &it->second : nullptr;
    }

    // Hostile attacker tracking
    [[nodiscard]] bool isHostileAttacker(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    void clearHostileAttackers() { hostileAttackers_.clear(); }

    // Forced faction reactions
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getForcedReactions() const { return forcedReactions_; }

    // Auto-attack timing state (read by GameHandler::update for retry/resend logic)
    bool& autoAttackOutOfRangeRef() { return autoAttackOutOfRange_; }
    float& autoAttackOutOfRangeTimeRef() { return autoAttackOutOfRangeTime_; }
    float& autoAttackRangeWarnCooldownRef() { return autoAttackRangeWarnCooldown_; }
    float& autoAttackResendTimerRef() { return autoAttackResendTimer_; }
    bool& autoAttackRetryPendingRef() { return autoAttackRetryPending_; }

    // Combat text creation (used by other handlers, e.g. spell handler for periodic damage)
    void addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId,
                       bool isPlayerSource, uint8_t powerType = 0,
                       uint64_t srcGuid = 0, uint64_t dstGuid = 0);

    // Spellsteal dedup (used by aura update handler)
    bool shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId);

    // Called from GameHandler::update() each frame
    void updateAutoAttack(float deltaTime);

    // --- Targeting ---
    /// False for creature corpses that are neither lootable nor skinnable -
    /// those cannot be selected. Living units, players and non-units are always selectable.
    [[nodiscard]] bool isSelectableUnit(uint64_t guid) const;
    void setTarget(uint64_t guid);
    void clearTarget();
    [[nodiscard]] std::shared_ptr<Entity> getTarget() const;
    void setFocus(uint64_t guid);
    void clearFocus();
    void setMouseoverGuid(uint64_t guid);
    void targetLastTarget();
    void targetEnemy(bool reverse);
    void targetFriend(bool reverse);

    /// The four narrower cycles WoW binds keys to, and which answered nothing.
    ///
    /// Same walk as the two above with a tighter test: only players, or only
    /// the people in your group. Bindings.xml gives each of them a key by
    /// default, so each was a key that did nothing at all.
    void targetNearestEnemyPlayer(bool reverse);
    void targetNearestFriendPlayer(bool reverse);
    void targetNearestPartyMember(bool reverse);
    void targetNearestRaidMember(bool reverse);

    /// Back to the last hostile, or the last friendly, target.
    ///
    /// TargetLastTarget remembers one target whatever it was; these remember one
    /// of each, which is what makes them useful - a healer's last friendly
    /// target survives half a fight's worth of tab-targeting.
    void targetLastEnemy();
    void targetLastFriend();
    void tabTarget(float playerX, float playerY, float playerZ);
    void assistTarget();

    // --- PvP ---
    void togglePvp();

    // --- The player's death ---
    // From dying to being alive again: the corpse, the offers to bring the
    // player back, and where a release goes.
    [[nodiscard]] bool isPlayerDead() const { return death_.dead; }
    [[nodiscard]] bool isPlayerGhost() const { return death_.ghost; }
    /// The graveyard a release would send the player to, in canonical space.
    /// False when the server has not named one, or has withdrawn it.
    bool getDeathReleaseLocation(uint32_t& mapId, glm::vec3& canonical) const;
    /// Distance (yards) from the player to their corpse, or -1 when its
    /// position is not known or it lies on another map.
    [[nodiscard]] float getCorpseDistance() const;
    /// The corpse's position in canonical coordinates (x north, y west); false
    /// when it is not known or lies on another map.
    bool getCorpseCanonicalPos(float& outX, float& outY) const;
    [[nodiscard]] bool canReclaimCorpse() const;
    /// Seconds left on the PvP corpse-reclaim delay, or 0 once it is over.
    [[nodiscard]] float getCorpseReclaimDelaySec() const;
    /// SMSG_PRE_RESURRECT arrived: Reincarnation, Twisting Nether.
    [[nodiscard]] bool canSelfRes() const { return death_.selfResAvailable; }
    /// The player accepted a resurrection or a spirit healer and waits for
    /// the server to carry it out; movement is held meanwhile.
    [[nodiscard]] bool resurrectPending() const { return death_.resurrectPending; }
    /// An offer to bring the player back stands, unanswered.
    [[nodiscard]] bool resurrectOfferStanding() const { return death_.resurrectRequestPending; }
    /// That offer is a spirit healer's (SMSG_SPIRIT_HEALER_CONFIRM), not a
    /// player's or a creature's resurrection.
    [[nodiscard]] bool resurrectOfferFromSpiritHealer() const { return death_.resurrectIsSpiritHealer; }
    /// What the resurrect offer said about itself: whether accepting brings
    /// resurrection sickness, and whether a reclaim delay applies. Both decide
    /// which of the three dialogs the interface raises.
    [[nodiscard]] bool resurrectHasSickness() const { return death_.resurrectHasSickness; }
    [[nodiscard]] bool resurrectHasTimer() const { return death_.resurrectHasTimer; }
    /// Seconds until an area spirit healer's next mass resurrection.
    [[nodiscard]] float getAreaSpiritHealerTime() const { return death_.areaSpiritHealerSeconds; }

    void releaseSpirit();
    void reclaimCorpse();
    void useSelfRes();
    void activateSpiritHealer(uint64_t npcGuid);
    void acceptResurrect();
    void declineResurrect();
    /// The spirit healer's prompt answered: activate it and close the offer.
    void acceptXpLoss();
    /// Ask an area spirit healer when the next mass resurrection is.
    void queryAreaSpiritHealer(uint64_t guid);
    /// Join that resurrection. Nothing happens without it.
    void queueAreaSpiritHeal();

    // What the rest of the game saw of the player's death.
    /// The player's own create block said health 0 or the dead flag.
    void playerLoggedInDead();
    /// The player's own create block carried the ghost flag.
    void playerLoggedInAsGhost();
    /// The player died where they stand: the corpse is put there.
    void playerDied(const char* source);
    /// Health came back above zero. True when the player was a ghost.
    bool playerHealthReturned();
    /// The dead flag went off the player.
    void playerAliveAgain();
    /// The ghost flag came on: the player released their spirit.
    void spiritReleased();
    /// The ghost flag went off: resurrected, and the corpse is gone.
    void resurrectedFromGhost();
    /// The player's own corpse came into view; with its position when the
    /// block carried one.
    void ownCorpseSeen(uint64_t guid, bool hasPosition, float serverX, float serverY, float serverZ);
    /// The world was entered: offers made before it no longer stand.
    void worldEntered();
    /// SMSG_NEW_WORLD took the player to a new place: the same.
    void arrivedInNewWorld();
    /// SMSG_NEW_WORLD on the same map, answering an accepted resurrection.
    void resurrectedInPlace();
    /// Once a frame: say when the ghost comes within reach of the corpse,
    /// and when it leaves it (CORPSE_IN_RANGE, CORPSE_OUT_OF_RANGE).
    void announceCorpseRange();

    // --- XP ---
    static uint32_t killXp(uint32_t playerLevel, uint32_t victimLevel);
    void handleXpGain(network::Packet& packet);

    // State management (for resets, entity cleanup)
    void resetAllCombatState();
    /// A character is being logged in: nothing of the last one's death stands.
    void beginCharacter() { death_ = {}; }
    void removeHostileAttacker(uint64_t guid);
    void clearCombatText();
    void removeCombatTextForGuid(uint64_t guid);

private:
    // Last tab-target press (steady-clock ms); a pause restarts the cycle
    // from the nearest enemy instead of resuming a stale rotation.
    uint64_t lastTabTargetMs_ = 0;

    // One remembered target of each kind, for targetLastEnemy/Friend. Written
    // by setTarget as the old target is let go, so they are the target before
    // this one rather than this one.
    uint64_t lastEnemyTargetGuid_ = 0;
    uint64_t lastFriendTargetGuid_ = 0;

    /// The walk both cycles do: everything alive within range that `wanted`
    /// accepts, nearest first, then step to the next one past the current
    /// target. Written out twice with two predicates before this, and the two
    /// had already drifted - one skipped the dead and the other did not, which
    /// is right both times and was not written down anywhere.
    void cycleTarget(bool reverse, const char* noneMessage,
                     const std::function<bool(uint64_t, Entity&)>& wanted);

    /// Whether a guid is in the player's group.
    [[nodiscard]] bool isGroupMemberGuid(uint64_t guid) const;

    // --- Packet handlers ---
    void handleAttackStart(network::Packet& packet);
    void handleAttackStop(network::Packet& packet);
    void handleAttackerStateUpdate(network::Packet& packet);
    void handleSpellDamageLog(network::Packet& packet);
    void handleSpellHealLog(network::Packet& packet);
    void handleSetForcedReactions(network::Packet& packet);
    void handleHealthUpdate(network::Packet& packet);
    void handlePowerUpdate(network::Packet& packet);
    void handleUpdateComboPoints(network::Packet& packet);
    void handlePvpCredit(network::Packet& packet);
    void handleProcResist(network::Packet& packet);
    void handleSpellDamageShield(network::Packet& packet);
    void handleSpellOrDamageImmune(network::Packet& packet);
    void handleResistLog(network::Packet& packet);
    void handleResurrectFailed(network::Packet& packet);

    void autoTargetAttacker(uint64_t attackerGuid);

    GameHandler& owner_;
    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    const core::CVarStore& cvars_;

    // --- Combat state ---
    bool autoAttacking_ = false;
    bool autoAttackRequested_ = false;
    bool autoAttackRetryPending_ = false;
    uint64_t autoAttackTarget_ = 0;
    bool autoAttackOutOfRange_ = false;
    float autoAttackOutOfRangeTime_ = 0.0f;
    float autoAttackRangeWarnCooldown_ = 0.0f;
    float autoAttackResendTimer_ = 0.0f;
    std::unordered_set<uint64_t> hostileAttackers_;

    /// The player's death, assigned whole when a character is logged in.
    struct PlayerDeath {
        bool dead = false;
        /// Released: PLAYER_FLAGS_GHOST.
        bool ghost = false;
        bool repopPending = false;
        uint64_t lastRepopRequestMs = 0;
        uint64_t pendingSpiritHealerGuid = 0;
        // The corpse, in server coordinates (x west, y north).
        bool corpsePositionValid = false;
        uint32_t corpseMapId = 0;
        float corpseX = 0.0f, corpseY = 0.0f, corpseZ = 0.0f;
        uint64_t corpseGuid = 0;
        /// Absolute time (ms since epoch) when the PvP reclaim delay ends;
        /// 0 when none applies.
        uint64_t corpseReclaimAvailableMs = 0;
        /// The last corpse proximity announced. canReclaimCorpse() is a
        /// predicate; CORPSE_IN_RANGE and CORPSE_OUT_OF_RANGE are its edges,
        /// and FrameXML raises its reclaim prompt from them.
        bool corpseInRangeAnnounced = false;
        /// Where the server says the spirit healer is - the graveyard a
        /// release sends the player to (SMSG_DEATH_RELEASE_LOC). Withdrawn by
        /// map id -1, which is how a resurrect clears the marker.
        bool deathReleaseValid = false;
        uint32_t deathReleaseMapId = 0;
        glm::vec3 deathReleaseCanonical{0.0f};
        // Offers to bring the player back.
        bool resurrectPending = false;
        bool resurrectRequestPending = false;
        /// true = SMSG_SPIRIT_HEALER_CONFIRM, false = SMSG_RESURRECT_REQUEST.
        bool resurrectIsSpiritHealer = false;
        uint64_t resurrectCasterGuid = 0;
        std::string resurrectCasterName;
        bool resurrectHasSickness = false;
        bool resurrectHasTimer = true;
        /// SMSG_PRE_RESURRECT: Reincarnation, Twisting Nether.
        bool selfResAvailable = false;
        uint64_t areaSpiritHealerGuid = 0;
        float areaSpiritHealerSeconds = 0.0f;
    };
    PlayerDeath death_;

    uint8_t comboPoints_ = 0;
    /// The hostile units Tab steps through, nearest first, and where it is.
    std::vector<uint64_t> tabCycleList_;
    int tabCycleIndex_ = -1;
    bool tabCycleStale_ = true;
    std::vector<CombatTextEntry> combatText_;
    static constexpr size_t MAX_COMBAT_LOG = 500;
    std::deque<CombatLogEntry> combatLog_;

    struct RecentSpellstealLogEntry {
        uint64_t casterGuid = 0;
        uint64_t victimGuid = 0;
        uint32_t spellId = 0;
        std::chrono::steady_clock::time_point timestamp{};
    };
    static constexpr size_t MAX_RECENT_SPELLSTEAL_LOGS = 32;
    std::deque<RecentSpellstealLogEntry> recentSpellstealLogs_;

    uint64_t lastMeleeSwingMs_ = 0;

    // unitGuid → sorted threat list (descending by threat value)
    std::unordered_map<uint64_t, std::vector<ThreatEntry>> threatLists_;
    /// Which mobs the player is currently top of the threat list for, so the
    /// aggro warning sounds on becoming that rather than on every update that
    /// says they still are. See the threat update handler.
    std::unordered_set<uint64_t> playerTopThreatOn_;
    /// Whether an aggro warning applies at all where the player is - the same
    /// question IsThreatWarningEnabled answers for the indicator, asked from
    /// C++ so the sound and the picture agree about when they are wanted.
    [[nodiscard]] bool threatWarningWanted() const;

    // Forced faction reactions
    std::unordered_map<uint32_t, uint8_t> forcedReactions_;
};

} // namespace game
} // namespace wowee
