#pragma once

#include "foundation/event_bus.hpp"
#include "game/world_packet_data.hpp"
#include "game/opcodes.hpp"
#include "game/group_defines.hpp"
#include "game/handler_types.hpp"
#include "network/packet.hpp"
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace core { class CVarStore; }
namespace contacts { class IContactsServerFeed; }
namespace who { class IWhoFeed; }
namespace duel { class IDuelsFeed; }
namespace petition { class IPetitionsFeed; }
namespace arena { class IArenaTeamsFeed; }
namespace battleground { class IBattlegroundsFeed; }
namespace lfg { class ILfgFeed; }
namespace calendar { class ICalendarFeed; }
namespace group { class IGroupServerFeed; class IGroup; }
namespace guild { class IGuildServerFeed; }
namespace guild_bank { class IGuildBankFeed; }
namespace game {

class PacketSender;
class FrameXmlEvents;
class ChatLog;
class EntityManager;
class PacketParsers;
struct Character;

/// What the social side asks of the rest of the game, and the few things it
/// has another part of it do. Grouped by the owner of each answer.
class SocialQueries {
public:
    virtual ~SocialQueries() = default;

    // ---- The character and the world in view ----
    [[nodiscard]] virtual uint64_t characterGuid() const = 0;
    /// The character logged in, or null between characters.
    [[nodiscard]] virtual const Character* activeCharacter() const = 0;
    [[nodiscard]] virtual uint64_t currentTargetGuid() const = 0;
    [[nodiscard]] virtual const EntityManager& entities() const = 0;
    [[nodiscard]] virtual uint32_t currentMapId() const = 0;
    [[nodiscard]] virtual bool battlegroundMap(uint32_t mapId) = 0;
    /// "player", "target", "party2", ... - the unit id FrameXML knows a guid
    /// by, or empty.
    [[nodiscard]] virtual std::string unitIdFor(uint64_t guid) const = 0;

    // ---- Other players ----
    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    virtual void requestPlayerName(uint64_t guid) = 0;
    [[nodiscard]] virtual uint8_t playerClassOf(uint64_t guid) const = 0;

    // ---- What the client's data says ----
    [[nodiscard]] virtual PacketParsers& packetParsers() = 0;
    [[nodiscard]] virtual std::string mapName(uint32_t mapId) const = 0;
    [[nodiscard]] virtual std::string areaName(uint32_t areaId) const = 0;
    [[nodiscard]] virtual std::string lfgDungeonName(uint32_t dungeonId) const = 0;
    [[nodiscard]] virtual const ItemQueryResponseData* itemInfo(uint32_t itemId) const = 0;
    virtual void ensureItemInfo(uint32_t itemId) = 0;
    [[nodiscard]] virtual uint8_t spellDispelType(uint32_t spellId) const = 0;
    /// Talent.dbc and TalentTab.dbc, read on the first ask - what an inspected
    /// character's talent bitfield is decoded against.
    [[nodiscard]] virtual const std::unordered_map<uint32_t, TalentTabEntry>& talentTabs() = 0;
    [[nodiscard]] virtual const std::unordered_map<uint32_t, TalentEntry>& talents() = 0;

    // ---- What another domain keeps ----
    /// A party member's auras, for the frames that read every unit's.
    virtual void mirrorAurasByGuid(uint64_t guid, const std::vector<AuraSlot>& auras) = 0;
    virtual void cacheInspectedPlayerEquipment(uint64_t guid,
                                               const std::array<uint32_t, 19>& itemEntries,
                                               const std::array<uint16_t, 19>& enchantIds) = 0;
    /// The equipment last inspected on this player; null when none is held.
    [[nodiscard]] virtual const std::array<uint32_t, 19>* inspectedItemEntries(uint64_t guid) const = 0;
    /// The player's own equipment is drawn again (the helm went on or off).
    virtual void markOnlineEquipmentDirty() = 0;
    virtual void addLocalChatMessage(const MessageChatData& message) = 0;
    /// The contact lists the friend, ignore and mute packets go to; null when
    /// the client was built without them.
    [[nodiscard]] virtual contacts::IContactsServerFeed* contactsFeed() = 0;
    /// Where SMSG_WHO's answer goes; null when the client was built without it.
    [[nodiscard]] virtual who::IWhoFeed* whoFeed() = 0;
    /// Where the duel packets go; null when the client was built without them.
    [[nodiscard]] virtual duel::IDuelsFeed* duelFeed() = 0;
    /// Where the charter packets go; null when the client was built without them.
    [[nodiscard]] virtual petition::IPetitionsFeed* petitionFeed() = 0;
    /// Where the arena team packets go; null when the client was built without them.
    [[nodiscard]] virtual arena::IArenaTeamsFeed* arenaFeed() = 0;
    /// Where the battleground packets go; null when the client was built without them.
    [[nodiscard]] virtual battleground::IBattlegroundsFeed* battlegroundFeed() = 0;
    /// Where the dungeon finder's packets go; null when the client was built without it.
    [[nodiscard]] virtual lfg::ILfgFeed* lfgFeed() = 0;
    /// Where the calendar's packets go; null when the client was built without it.
    [[nodiscard]] virtual calendar::ICalendarFeed* calendarFeed() = 0;
    /// Where the party and raid packets go; null when the client was built
    /// without the group.
    [[nodiscard]] virtual group::IGroupServerFeed* groupFeed() = 0;
    /// The group as it stands, for a member's stats; null as above.
    [[nodiscard]] virtual const group::IGroup* group() const = 0;
    /// Where the guild's packets go; null when the client was built without
    /// the guild.
    [[nodiscard]] virtual guild::IGuildServerFeed* guildFeed() = 0;
    /// Where the guild bank's logs and the balance the guild reports go;
    /// null when the client was built without the vault.
    [[nodiscard]] virtual guild_bank::IGuildBankFeed* guildBankFeed() = 0;

    // ---- The appearance switches the realm keeps with the character ----
    [[nodiscard]] virtual bool showingHelm() const = 0;
    [[nodiscard]] virtual bool showingCloak() const = 0;
    virtual void setShowingHelm(bool shown) = 0;
    virtual void setShowingCloak(bool shown) = 0;
};

/// What the social side of the game keeps for one character: its party and
/// guild, the invitations and challenges put to it, its queues and its saved
/// instances. SocialHandler is this and assigns it whole
/// in beginCharacter: the server sends
/// the next character's own, and a duel or an invite put to the last one is
/// not the next one's to answer.
struct SocialCharacterState {
    // Inspect
    InspectResult inspectResult_;

    // Time played
    uint32_t totalTimePlayed_ = 0;
    uint32_t levelTimePlayed_ = 0;


    /// Points on the minimap the party pinged, while they last.
    std::vector<MinimapPing> minimapPings_;

    // A summon put to the player, waiting for an answer.
    bool        pendingSummonRequest_ = false;
    uint64_t    summonerGuid_         = 0;
    std::string summonerName_;
    float       summonTimeoutSec_     = 0.0f;

    // The outdoor battlefield (Wintergrasp) invitation and the battle it is for.
    bool     bfMgrInvitePending_ = false;
    uint32_t bfMgrZoneId_        = 0;
    uint32_t bfMgrBattleId_      = 0;

    // Instance
    std::vector<InstanceLockout> instanceLockouts_;
    uint32_t instanceDifficulty_ = 0;
    bool instanceIsHeroic_ = false;

};

class SocialHandler : private SocialCharacterState {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    SocialHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                  ChatLog& chat, SocialQueries& queries, const core::CVarStore& cvars);

    void registerOpcodes(DispatchTable& table);
    /// A character is being logged in: its party, guild, queues, the offers
    /// put to it and its saved instances begin, here and in each social
    /// context.
    void beginCharacter();

    // ---- Structs (aliased from handler_types.hpp) ----

    using InspectArenaTeam = game::InspectArenaTeam;

    using InspectResult = game::InspectResult;



    using InstanceLockout = game::InstanceLockout;

    // ---- Public API ----

    // Inspection
    void inspectTarget();
    /// Inspect any player by guid; inspectTarget is this with the current target.
    void inspectUnit(uint64_t guid);
    /// Ask for the honour figures behind the inspect window's PvP tab. A
    /// separate request from the inspect itself, and answered on its own
    /// opcode, which is why the tab asks for it when it opens.
    void requestInspectHonorData(uint64_t guid);
    [[nodiscard]] const InspectResult* getInspectResult() const {
        return inspectResult_.guid ? &inspectResult_ : nullptr;
    }

    // Server info / who
    /// Ask the server for the time. `announce` prints it to chat, which is
    /// what /time wants and what the login-time query must not do.
    void queryServerTime(bool announce = false);
    /// Seconds until the daily quest reset, or 0 if the server has not said.
    /// Counted down from when the answer arrived rather than stored raw, since
    /// the reply is asked for once and read for the rest of the session.
    [[nodiscard]] uint32_t getSecondsUntilDailyReset() const;
    void requestPlayedTime();
    [[nodiscard]] uint32_t getTotalTimePlayed() const { return totalTimePlayed_; }
    [[nodiscard]] uint32_t getLevelTimePlayed() const { return levelTimePlayed_; }

    // Random roll
    void randomRoll(uint32_t minRoll = 1, uint32_t maxRoll = 100);

    // Logout. exitAfterLogout distinguishes /quit and /exit, which leave the game
    // entirely, from /logout and /camp, which drop back to character select.
    void requestLogout(bool exitAfterLogout = false);
    void cancelLogout();
    [[nodiscard]] bool  isLoggingOut()        const { return loggingOut_; }
    [[nodiscard]] float getLogoutCountdown()  const { return logoutCountdown_; }
    [[nodiscard]] bool  isLogoutToExit()      const { return exitAfterLogout_; }

    void takeInboxTextItem(uint32_t mailId);
    void reportPlayer(uint64_t targetGuid, const std::string& reason);

    /// Report a mail as spam. Type 0 of CMSG_COMPLAIN; the three values after
    /// the sender are a zero, the mail's own id, and another zero.
    void reportMailSpam(uint64_t senderGuid, uint32_t mailId);
    void channelModeration(Opcode op, const std::string& channelName,
                           const std::string& targetName,
                           bool allowEmptyTarget = false);
    void requestRaidInfo();
    void setSavedInstanceExtend(uint32_t mapId, uint32_t difficulty, bool extend);

    // Instance lockouts
    [[nodiscard]] const std::vector<InstanceLockout>& getInstanceLockouts() const { return instanceLockouts_; }

    // Instance difficulty
    [[nodiscard]] uint32_t getInstanceDifficulty() const { return instanceDifficulty_; }
    [[nodiscard]] bool isInstanceHeroic() const { return instanceIsHeroic_; }
    // Whether the player is in an instance is GameHandler::isInInstance, from
    // Map.dbc. A difficulty packet arrives in the open world too, and the flag
    // it used to set here was never read.

    // Minimap ping
    void sendMinimapPing(float wowX, float wowY);
    [[nodiscard]] const std::vector<MinimapPing>& minimapPings() const { return minimapPings_; }
    /// Let pings age by a frame; the ones past their lifetime go.
    void ageMinimapPings(float deltaTime);

    [[nodiscard]] const std::string& summonerName() const { return summonerName_; }
    [[nodiscard]] float summonTimeoutSec() const { return summonTimeoutSec_; }
    [[nodiscard]] uint32_t bfMgrBattleId() const { return bfMgrBattleId_; }

    // Summon request
    void handleSummonRequest(network::Packet& packet);
    /// CMSG_SUMMON_RESPONSE: the summoner's guid, then yes or no.
    void sendSummonResponse(bool accept);
    void acceptSummon();
    void declineSummon();

    // Battlefield Manager
    void acceptBfMgrInvite();
    void declineBfMgrInvite();
    void respondBfMgrQueueInvite(uint32_t battleId, bool accept);
    void requestBfMgrExit(uint32_t battleId);

    // ---- Methods moved from GameHandler ----
    void sendSetDifficulty(uint32_t difficulty, bool raid = false);
    void toggleHelm();
    void toggleCloak();
    void sendAlterAppearance(uint32_t hairStyleEntry, uint32_t hairColor,
                             uint32_t facialHairEntry, uint32_t skinColorEntry);
    void deleteGmTicket();
    void requestGmTicket();
    /// Ask whether the GM ticket queue is accepting tickets.
    void requestGmSystemStatus();

    // Utility methods for delegation from GameHandler
    void updateLogoutCountdown(float deltaTime);
    void resetTransferState();
    InspectResult& mutableInspectResult() { return inspectResult_; }
    void setEncounterUnitGuid(uint32_t slot, uint64_t guid) {
        if (slot < kMaxEncounterSlots) encounterUnitGuids_[slot] = guid;
    }

    // The bind-or-leave question the server asks on entering an instance that
    // is already under way. It carries a countdown, and the answer is the
    // player's: accept and be saved, decline and go to the graveyard. Nothing
    // happens until one of the two is sent, so this is held rather than
    // answered here.
    struct InstanceLockPrompt {
        bool active = false;
        float secondsLeft = 0.0f;
        bool previouslySaved = false;
        uint32_t completedEncounterMask = 0;
    };
    [[nodiscard]] const InstanceLockPrompt& getInstanceLockPrompt() const { return instanceLock_; }
    void respondInstanceLock(bool accept);

    // Encounter unit tracking
    static constexpr uint32_t kMaxEncounterSlots = 5;
    [[nodiscard]] uint64_t getEncounterUnitGuid(uint32_t slot) const {
        return (slot < kMaxEncounterSlots) ? encounterUnitGuids_[slot] : 0;
    }




private:
    // ---- Packet handlers ----
    void handleInspectResults(network::Packet& packet);
    void sendBfMgrResponse(Opcode op, uint32_t battleId, bool accept, bool withFlag);
    void handleQueryTimeResponse(network::Packet& packet);
    void handlePlayedTime(network::Packet& packet);
    void handleRandomRoll(network::Packet& packet);
    void handleLogoutResponse(network::Packet& packet);
    void handleLogoutComplete(network::Packet& packet);
    void handleGroupInvite(network::Packet& packet);
    void handleGroupList(network::Packet& packet);
    void handlePartyMemberStats(network::Packet& packet, bool isFull);
    void handlePetitionShowlist(network::Packet& packet);
    void handlePetitionQueryResponse(network::Packet& packet);
    void handlePetitionShowSignatures(network::Packet& packet);
    void handlePetitionSignResults(network::Packet& packet);
    void handleRaidInstanceInfo(network::Packet& packet);
    void handleInstanceDifficulty(network::Packet& packet);
    void handleLfgJoinResult(network::Packet& packet);
    void handleLfgQueueStatus(network::Packet& packet);
    void handleLfgProposalUpdate(network::Packet& packet);
    void handleLfgRoleCheckUpdate(network::Packet& packet);
    void handleLfgUpdatePlayer(network::Packet& packet);
    void handleLfgUpdateParty(network::Packet& packet);
    /// The first dungeon an update names, which the queue window shows.
    std::optional<uint32_t> readLfgFirstDungeon(network::Packet& packet);
    void handleLfgPlayerInfo(network::Packet& packet);
    void handleLfgPlayerReward(network::Packet& packet);
    void handleLfgBootProposalUpdate(network::Packet& packet);
    void handleLfgTeleportDenied(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);

    void raiseUiError(const std::string& message);
    /// The outdoor battlefield opcodes (Wintergrasp), and the summon's cancel.
    void registerBattlefieldOpcodes(DispatchTable& table);

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    SocialQueries& queries_;

    // ---- State (the character's is SocialCharacterState) ----

    // Logout
    bool  loggingOut_        = false;
    bool  exitAfterLogout_   = false;
    float logoutCountdown_   = 0.0f;
    /// events::LogoutCommitted went out for this logout.
    bool  logoutCommitted_   = false;
    /// Announce, once per logout, that it can no longer be cancelled.
    void commitLogout();

    /// SMSG_QUERY_TIME_RESPONSE's second word and when it landed. Zero means
    /// no answer yet, which is different from "the reset is now".
    uint32_t dailyResetOffset_ = 0;
    time_t   dailyResetReceivedAt_ = 0;
    bool     announceServerTime_ = false;

    // Encounter units and the instance's bind question: the map's, ended
    // with it (resetTransferState).
    std::array<uint64_t, kMaxEncounterSlots> encounterUnitGuids_ = {};
    InstanceLockPrompt instanceLock_;

    /// What this handler hears of the other domains. Last, so it goes first.
    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace game
} // namespace wowee
