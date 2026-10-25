#pragma once

#include "core/event_bus.hpp"
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
    virtual void setGuildBankMoney(uint64_t money) = 0;
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

    // ---- The appearance switches the realm keeps with the character ----
    [[nodiscard]] virtual bool showingHelm() const = 0;
    [[nodiscard]] virtual bool showingCloak() const = 0;
    virtual void setShowingHelm(bool shown) = 0;
    virtual void setShowingCloak(bool shown) = 0;
};

/// What the social side of the game keeps for one character: its party and
/// guild, the invitations and challenges put to it, its queues and its saved
/// instances. SocialHandler is this and assigns it whole
/// when a character logs in (events::CharacterLoggingIn): the server sends
/// the next character's own, and a duel or an invite put to the last one is
/// not the next one's to answer.
struct SocialCharacterState {
    // Inspect
    InspectResult inspectResult_;

    // Time played
    uint32_t totalTimePlayed_ = 0;
    uint32_t levelTimePlayed_ = 0;


    // Guild. The last character's is not the next one's: logging out of a
    // guilded character into a guildless one used to leave the name, and
    // everything that asks "am I in a guild" answered yes.
    /// Membership the server has confirmed this session, for a guild joined
    /// after login - which no record built at login can know about.
    bool inGuild_ = false;
    /// What the invite called it, kept until the guild names itself.
    std::string joinedGuildName_;
    std::string guildName_;
    std::vector<std::string> guildRankNames_;
    GuildRosterData guildRoster_;
    GuildInfoData guildInfoData_;
    GuildQueryResponseData guildQueryData_;
    bool hasGuildRoster_ = false;
    bool pendingGuildInvite_ = false;
    std::string pendingGuildInviterName_;
    std::string pendingGuildInviteGuildName_;

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
    std::vector<GuildEventLogEntry> guildEventLog_;
    /// Six item tabs and the money log after them.
    std::array<std::vector<GuildBankLogEntry>, 7> guildBankLogs_;
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

    // Guild
    void requestGuildInfo();
    void requestGuildRoster();
    /// Ask for the guild's event log. The reply is the same opcode.
    void requestGuildEventLog();
    /// Ask for one bank tab's log. Tab six is the money log, which is what the
    /// server means by GUILD_BANK_MAX_TABS.
    void requestGuildBankLog(uint8_t tab);

    static constexpr uint8_t kGuildBankMoneyTab = 6;
    [[nodiscard]] const std::vector<GuildBankLogEntry>& getGuildBankLog(uint8_t tab) const {
        static const std::vector<GuildBankLogEntry> empty;
        return (tab < guildBankLogs_.size()) ? guildBankLogs_[tab] : empty;
    }
    [[nodiscard]] const std::vector<GuildEventLogEntry>& getGuildEventLog() const { return guildEventLog_; }
    void setGuildInfoText(const std::string& text);
    void takeInboxTextItem(uint32_t mailId);
    void setGuildMotd(const std::string& motd);
    void promoteGuildMember(const std::string& playerName);
    void demoteGuildMember(const std::string& playerName);
    void leaveGuild();
    void inviteToGuild(const std::string& playerName);
    void kickGuildMember(const std::string& playerName);
    void disbandGuild();
    void setGuildLeader(const std::string& name);
    void setGuildPublicNote(const std::string& name, const std::string& note);
    void setGuildOfficerNote(const std::string& name, const std::string& note);
    void acceptGuildInvite();
    void declineGuildInvite();
    void queryGuildInfo(uint32_t guildId);
    void createGuild(const std::string& guildName);
    void addGuildRank(const std::string& rankName);
    void delGuildRank();
    void saveGuildRank(uint32_t rankId, uint32_t rights, const std::string& rankName,
                       uint32_t goldLimit, const uint32_t* tabRights, const uint32_t* tabSlots);
    void deleteGuildRank();

    // Guild state accessors
    [[nodiscard]] bool isInGuild() const;
    [[nodiscard]] const std::string& getGuildName() const { return guildName_; }
    [[nodiscard]] const GuildRosterData& getGuildRoster() const { return guildRoster_; }
    [[nodiscard]] bool hasGuildRoster() const { return hasGuildRoster_; }

    /// The guild this character is in, by id, whichever record knows it.
    ///
    /// The character list is the record at login and goes stale the moment a
    /// guild is joined or left mid-session; the player's own PLAYER_GUILDID
    /// field is the one the server keeps current. Zero for a player in none.
    [[nodiscard]] uint32_t ownGuildId() const;

    [[nodiscard]] const std::vector<std::string>& getGuildRankNames() const { return guildRankNames_; }
    /// The rights bitmask of the player's own guild rank, or 0 when it is not
    /// known. The roster carries one per rank and the member row names which
    /// rank each member holds; both were parsed and stored and neither was
    /// being read.
    /// Where the player sits in the guild, as an index into the rank list.
    /// 0xFFFFFFFF when the roster has not arrived or the player is not in it -
    /// distinct from rank zero, which is the guild master.
    [[nodiscard]] uint32_t getPlayerGuildRankIndex() const;
    [[nodiscard]] uint32_t getPlayerGuildRankRights() const;
    [[nodiscard]] bool hasPendingGuildInvite() const { return pendingGuildInvite_; }
    [[nodiscard]] const std::string& getPendingGuildInviterName() const { return pendingGuildInviterName_; }
    [[nodiscard]] const std::string& getPendingGuildInviteGuildName() const { return pendingGuildInviteGuildName_; }
    [[nodiscard]] const GuildInfoData& getGuildInfoData() const { return guildInfoData_; }
    [[nodiscard]] const GuildQueryResponseData& getGuildQueryData() const { return guildQueryData_; }
    [[nodiscard]] bool hasGuildInfoData() const { return guildInfoData_.isValid(); }

    // Guild name lookup
    const std::string& lookupGuildName(uint32_t guildId);
    [[nodiscard]] uint32_t getEntityGuildId(uint64_t guid) const;

    void reportPlayer(uint64_t targetGuid, const std::string& reason);

    /// Report a mail as spam. Type 0 of CMSG_COMPLAIN; the three values after
    /// the sender are a zero, the mail's own id, and another zero.
    void reportMailSpam(uint64_t senderGuid, uint32_t mailId);
    void setGuildBankTabText(uint8_t tab, const std::string& text);
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
    void setStandState(uint8_t standState);
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
    void handleGuildBankLog(network::Packet& packet);
    void sendBfMgrResponse(Opcode op, uint32_t battleId, bool accept, bool withFlag);
    void handleQueryTimeResponse(network::Packet& packet);
    void handlePlayedTime(network::Packet& packet);
    void handleRandomRoll(network::Packet& packet);
    void handleLogoutResponse(network::Packet& packet);
    void handleLogoutComplete(network::Packet& packet);
    void handleGroupInvite(network::Packet& packet);
    void handleGroupList(network::Packet& packet);
    void handlePartyMemberStats(network::Packet& packet, bool isFull);
    void handleGuildInfo(network::Packet& packet);
    void handleGuildRoster(network::Packet& packet);
    void handleGuildQueryResponse(network::Packet& packet);
    void handleGuildEvent(network::Packet& packet);
    // Updates a roster member's cached online flag; returns true only on a real change so
    // the login-time SIGNED_ON flood (state unchanged) is suppressed from guild chat.
    bool guildMemberOnlineTransition(const std::string& name, bool nowOnline);
    void handleGuildInvite(network::Packet& packet);
    void handleGuildCommandResult(network::Packet& packet);
    void handlePetitionShowlist(network::Packet& packet);
    void handlePetitionQueryResponse(network::Packet& packet);
    void handleGuildEventLog(network::Packet& packet);
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
    void loadGuildNameCache();
    void saveGuildNameCache() const;
    void rememberGuildName(uint32_t guildId, const std::string& guildName);

    void raiseUiError(const std::string& message);
    /// The outdoor battlefield opcodes (Wintergrasp), and the summon's cancel.
    void registerBattlefieldOpcodes(DispatchTable& table);

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatLog& chat_;
    SocialQueries& queries_;
    const core::CVarStore& cvars_;

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

    // Guild names by id, which outlive any one character.
    std::unordered_map<uint32_t, std::string> guildNameCache_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> pendingGuildNameQueries_;

    // Encounter units and the instance's bind question: the map's, ended
    // with it (resetTransferState).
    std::array<uint64_t, kMaxEncounterSlots> encounterUnitGuids_ = {};
    InstanceLockPrompt instanceLock_;

    /// What this handler hears of the session: the character's state begins
    /// anew at each login (events::CharacterLoggingIn). Last, so it goes first.
    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace game
} // namespace wowee
