#pragma once

#include <deque>

#include "core/event_bus.hpp"
#include "game/chat_filters.hpp"
#include "game/chat_log.hpp"

#include "game/world_packet_data.hpp"
#include "game/opcode_table.hpp"
#include "game/handler_types.hpp"
#include "network/packet.hpp"
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

namespace wowee {
namespace core { class CVarStore; }
namespace game {

class PacketSender;
class FrameXmlEvents;
class PacketParsers;
class EmoteTable;
struct Character;
struct GroupListData;
struct GuildRosterData;
struct TextSubject;

/// What chat asks of the rest of the game: who the player is and where, who
/// another player is, and the tables a line is dressed from. Grouped by the
/// owner of each answer.
class ChatQueries {
public:
    virtual ~ChatQueries() = default;

    // ---- The character ----
    [[nodiscard]] virtual uint64_t characterGuid() const = 0;
    /// The character logged in, or null between characters.
    [[nodiscard]] virtual const Character* activeCharacter() const = 0;
    [[nodiscard]] virtual uint32_t currentMapId() const = 0;
    [[nodiscard]] virtual const MovementInfo& playerMovement() const = 0;

    // ---- Other players ----
    /// The name the game knows a guid by, or empty.
    [[nodiscard]] virtual std::string playerName(uint64_t guid) const = 0;
    virtual void requestPlayerName(uint64_t guid) = 0;
    /// Class, race and gender as a name query answered them; 0 (gender 2)
    /// while unknown.
    [[nodiscard]] virtual uint8_t playerClassOf(uint64_t guid) const = 0;
    [[nodiscard]] virtual uint8_t playerRaceOf(uint64_t guid) const = 0;
    [[nodiscard]] virtual uint8_t playerGenderOf(uint64_t guid) const = 0;
    [[nodiscard]] virtual const GroupListData& party() const = 0;
    [[nodiscard]] virtual const GuildRosterData& guildRoster() const = 0;
    /// The player's guild rank's rights, or nothing while the rank is not
    /// known.
    [[nodiscard]] virtual std::optional<uint32_t> guildRankRights() const = 0;

    // ---- What the client's data says ----
    [[nodiscard]] virtual PacketParsers& packetParsers() = 0;
    /// Languages.dbc's name for a language ("Common"), "Universal" for 0.
    [[nodiscard]] virtual std::string languageName(uint32_t languageId) const = 0;
    [[nodiscard]] virtual std::string achievementName(uint32_t achievementId) = 0;
};

/// The chat log is this handler's, and the domains that write a line to it
/// are handed it as a ChatLog.
class ChatHandler : public ChatLog {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    ChatHandler(PacketSender& sender, FrameXmlEvents& frameXml, core::EventBus& events,
                ChatQueries& queries, const core::CVarStore& cvars, const EmoteTable& emotes);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---
    void sendChatMessage(ChatType type, const std::string& message, const std::string& target = "");
    void sendAddonMessage(ChatType type, const std::string& message, const std::string& target = "");
    void sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid = 0);
    void joinChannel(const std::string& channelName, const std::string& password = "");
    void leaveChannel(const std::string& channelName);
    /// Ask the server for a channel's members; the answer arrives as
    /// SMSG_CHANNEL_LIST and fires CHANNEL_ROSTER_UPDATE.
    void requestChannelList(const std::string& channelName);
    [[nodiscard]] std::string getChannelByIndex(int index) const;
    [[nodiscard]] int getChannelIndex(const std::string& channelName) const;
    [[nodiscard]] const std::vector<std::string>& getJoinedChannels() const {
        return character_.joinedChannels;
    }
    /// Whether the player owns this channel - the one who created it, or who
    /// it passed to. SMSG_CHANNEL_NOTIFY says so with OWNER_CHANGED, which
    /// carries the new owner's guid: AzerothCore's Channel::MakeOwnerChanged
    /// writes _ownerGUID into it.
    [[nodiscard]] bool ownsChannel(const std::string& name) const {
        return character_.ownedChannels.count(name) != 0;
    }
    /// Who sent the line with this id, or zero if it is not remembered.
    [[nodiscard]] uint64_t chatLineSender(uint32_t lineId) const {
        for (const auto& [id, guid] : character_.lineSenders)
            if (id == lineId) return guid;
        return 0;
    }

    /// The roster of a channel by name, empty until a list is asked for. The
    /// list used to be printed to chat and dropped, so the channel panel had
    /// no members to show and reported every channel as having none.
    [[nodiscard]] const std::vector<ChannelMember>& getChannelRoster(const std::string& channel) const {
        static const std::vector<ChannelMember> empty;
        auto it = character_.channelRosters.find(channel);
        return (it != character_.channelRosters.end()) ? it->second : empty;
    }
    void autoJoinDefaultChannels();
    void addLocalChatMessage(const MessageChatData& msg);

    /// Put a line of a given kind into chat, locally.
    ///
    /// The kind matters: CHAT_MSG_MONEY, CHAT_MSG_COMBAT_XP_GAIN and their like
    /// are chat messages, and the interface's handler reads twelve arguments
    /// off every one of them - arg4 is a channel name and it calls strlen on it
    /// without checking. Firing one of these with just the text raised there and
    /// took the rest of the handler with it, so the line never appeared at all.
    void addLocalChatLine(ChatType type, const std::string& message) override;
    void addSystemChatMessage(const std::string& message) override;

    /// Announces a message as CHAT_MSG_<TYPE> with WoW's argument order. Both
    /// the server path and the local one go through here, because an interface
    /// cannot tell the difference and should not have to.
    void fireChatEvent(const MessageChatData& msg);
    void toggleAfk(const std::string& message);
    void toggleDnd(const std::string& message);
    void replyToLastWhisper(const std::string& message);
    [[nodiscard]] bool isAfk() const { return character_.afk; }
    [[nodiscard]] bool isDnd() const { return character_.dnd; }
    /// Who /r answers: the last player to whisper, by the name the whisper
    /// came with or, when it came with none, the one the name cache has for
    /// that guid since.
    [[nodiscard]] std::string getLastWhisperSender() const;

    // ---- Methods moved from GameHandler ----
    void submitGmTicket(const std::string& text);
    /// Replace the text of the ticket already open.
    void updateGmTicket(const std::string& text);

    /// Release anything whose wait has run out, named or not. A line nobody
    /// answers for is still a line the player should see.
    void expireChatAwaitingName();
    void handleMotd(network::Packet& packet);

    // --- State accessors ---
    [[nodiscard]] const std::deque<MessageChatData>& getChatHistory() const { return character_.history; }
    /// /clear: the player emptying their own log.
    void clearChatHistory() { character_.history.clear(); }
    [[nodiscard]] size_t getMaxChatHistory() const { return maxChatHistory_; }
    void setMaxChatHistory(size_t n) { maxChatHistory_ = n; }

    /// SMSG_AREA_TRIGGER_MESSAGE texts not yet shown as a notice, oldest
    /// first; the notice takes them.
    [[nodiscard]] bool hasAreaTriggerMessage() const { return !character_.areaTriggerMessages.empty(); }
    std::string takeAreaTriggerMessage();

    // Chat auto-join settings (aliased from handler_types.hpp)
    using ChatAutoJoin = game::ChatAutoJoin;
    ChatAutoJoin chatAutoJoin;

private:
    /// A line on the wire, with its local echo. sendChatMessage is the player
    /// speaking - which also ends AFK - and calls this; an answer given for
    /// the player, such as the AFK auto-reply, calls it directly.
    void sendLine(ChatType type, const std::string& message, const std::string& target);

    /// A chat line waiting for a name: its sender's, or that of the player an
    /// NPC's line is written for. See deliverChatMessage.
    struct ChatAwaitingName {
        MessageChatData data;
        uint64_t guid = 0;
        double deadline = 0.0;
    };
    /// Long enough for a name query to come back on a working connection,
    /// short enough that a line does not feel withheld.
    static constexpr double kNameWaitSeconds = 1.0;
    // --- Packet handlers ---
    void handleMessageChat(network::Packet& packet);
    /// Deliver a parsed chat line: resolve its sender, filter it, store it and
    /// tell the interface. Split from handleMessageChat so a line held back
    /// for its sender's name can be run through it again once the name lands.
    void deliverChatMessage(MessageChatData data, bool alreadyWaited);
    /// A player's name from whatever already holds it - the name cache, the
    /// player nearby, the party, the guild roster. Empty when none does.
    [[nodiscard]] std::string knownPlayerName(uint64_t guid) const;
    /// Who an NPC's line fills its $-tokens in for: the receiver the packet
    /// names, or the logged-in character when it names nobody.
    TextSubject monsterLineSubject(const MessageChatData& data);

    void handleTextEmote(network::Packet& packet);
    void handleChannelNotify(network::Packet& packet);
    void handleChannelList(network::Packet& packet);
    void handleUserlistAdd(network::Packet& packet);
    void handleUserlistRemove(network::Packet& packet);
    void applyUserlistChange(const std::string& chanName, uint64_t guid,
                             uint8_t memberFlags, bool removing);
    void initializeChatLog();
    void logChatMessage(const MessageChatData& msg, const char* source);
    void raiseUiError(const std::string& message);
    /// Release the lines held for `guid`, now that its name is known.
    void flushChatAwaitingName(uint64_t guid);
    /// The logged-in character as the $-tokens of an NPC's line read them.
    [[nodiscard]] TextSubject readerSubject() const;

    PacketSender& sender_;
    FrameXmlEvents& frameXml_;
    core::EventBus& events_;
    ChatQueries& queries_;
    const core::CVarStore& cvars_;
    const EmoteTable& emotes_;

    /// The character's conversation: what was said to them, where they are
    /// listening, whether they are away and who /r answers. Assigned whole
    /// when a character logs in (events::CharacterLoggingIn), so none of it
    /// reaches the next character - the server joins a character to its own
    /// channels and nobody has whispered them yet.
    struct Conversation {
        std::deque<MessageChatData> history;
        /// What has just been said, for the spam filter. Short on purpose:
        /// this is looking for a line pasted again a moment later, not keeping
        /// a record of the conversation. See repeatsRecentLine.
        std::deque<RecentChatLine> recentLines;
        /// Who sent the message a chat line id names, newest last.
        ///
        /// Every chat event carries a line id as its eleventh argument, and
        /// FrameXML builds it into the player link on the name - so a
        /// right-click on that name hands the id back and expects the client
        /// to know which message it was. Bounded: only recent lines can be
        /// reported anyway.
        std::deque<std::pair<uint32_t, uint64_t>> lineSenders;
        uint32_t nextLineId = 1;
        std::deque<ChatAwaitingName> awaitingName;
        std::vector<std::string> joinedChannels;
        std::set<std::string> ownedChannels;
        std::unordered_map<std::string, std::vector<ChannelMember>> channelRosters;
        // Away and do-not-disturb, with what the auto-reply says for each.
        bool afk = false;
        bool dnd = false;
        std::string afkMessage;
        std::string dndMessage;
        /// Whisperers already answered while away, so two clients both away
        /// do not answer each other forever. Cleared when AFK/DND ends.
        std::unordered_set<std::string> autoRepliedSenders;
        /// The last player to whisper, for /r. The guid as well as the name:
        /// a whisper can arrive before its sender's name does.
        std::string lastWhisperSender;
        uint64_t lastWhisperSenderGuid = 0;
        std::deque<std::string> areaTriggerMessages;
    };
    Conversation character_;

    size_t maxChatHistory_ = 100;
    /// Monotonic across characters: the interface tells new lines by it.
    uint64_t chatUidCounter_ = 0;
    bool chatLogEnabled_ = false;
    bool chatLogInitialized_ = false;
    std::string chatLogPath_;
    std::ofstream chatLogStream_;

    std::vector<core::EventBus::Subscription> subscriptions_;
};

} // namespace game
} // namespace wowee
