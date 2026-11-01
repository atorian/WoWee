#include "game/guild_gateway.hpp"

#include "core/cvar_store.hpp"
#include "core/logger.hpp"
#include "game/chat_log.hpp"
#include "game/entity.hpp"
#include "game/frame_xml_events.hpp"
#include "game/handler_types.hpp"
#include "game/opcode_table.hpp"
#include "game/opcodes.hpp"
#include "game/packet_sender.hpp"
#include "game/social_handler.hpp"
#include "game/ui_errors.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"
#include "guild/domain/events.hpp"
#include "guild_bank/services/guild_bank.hpp"
#include "network/packet.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace wowee::game {

namespace {

std::filesystem::path guildNameCachePath() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) {
        return std::filesystem::path(appData) / "wowee" / "guild_names.tsv";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".wowee" / "guild_names.tsv";
    }
#endif
    return std::filesystem::path("guild_names.tsv");
}

std::string commandError(const guild::CommandFailed& e) {
    const char* text = nullptr;
    switch (e.errorCode) {
        case 2:  text = "You are not in a guild."; break;
        case 4:  text = "No player named \"%s\" is online."; break;
        case 11: text = "\"%s\" is already in a guild."; break;
        case 13: text = "You are already in a guild."; break;
        case 14: text = "\"%s\" has already been invited to a guild."; break;
        case 16: case 17: text = "You are not the guild leader."; break;
        case 22: text = "That player is ignoring you."; break;
        default: break;
    }
    if (!text) {
        std::string msg = "Guild command failed";
        if (!e.name.empty()) msg += " for " + e.name;
        return msg + " (error " + std::to_string(e.errorCode) + ")";
    }
    std::string msg = text;
    if (auto pos = msg.find("%s"); pos != std::string::npos)
        msg.replace(pos, 2, e.name.empty() ? std::string("that player") : e.name);
    return msg;
}

} // namespace

bool GuildGateway::canSend() const {
    return sender_.characterInWorld();
}

// A guild's name may be asked for at the character screen too: the socket is
// connected and the server accepts CMSG_GUILD_QUERY before login.
bool GuildGateway::canQuery() const {
    return sender_.connected();
}

void GuildGateway::requestInfo() {
    sender_.send(GuildInfoPacket::build());
}

void GuildGateway::requestRoster() {
    sender_.send(GuildRosterPacket::build());
    // Said in the log, not to the player. This is asked for far more often
    // than a person would ask for it: the guild asks after every change that
    // moves its roster, and the interface whenever the mail frame or the
    // friends frame opens with an empty one. Every one of those put a line in
    // chat that the real client never writes.
    LOG_DEBUG("Requested guild roster");
}

// MSG_GUILD_EVENT_LOG_QUERY: asked with no payload; the reply is the same
// opcode.
void GuildGateway::requestEventLog() {
    network::Packet packet(wireOpcode(Opcode::MSG_GUILD_EVENT_LOG_QUERY));
    sender_.send(packet);
}

void GuildGateway::queryGuild(uint32_t guildId) {
    sender_.send(GuildQueryPacket::build(guildId));
}

// The longer "guild information" text, which is a different field from the
// message of the day and has its own opcode.
void GuildGateway::sendInfoText(const std::string& text) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_INFO_TEXT));
    packet.writeString(text);
    sender_.send(packet);
}

void GuildGateway::sendMotd(const std::string& motd) {
    sender_.send(GuildMotdPacket::build(motd));
}

void GuildGateway::sendPromote(const std::string& name) {
    sender_.send(GuildPromotePacket::build(name));
}

void GuildGateway::sendDemote(const std::string& name) {
    sender_.send(GuildDemotePacket::build(name));
}

void GuildGateway::sendLeave() {
    sender_.send(GuildLeavePacket::build());
}

void GuildGateway::sendInvite(const std::string& name) {
    sender_.send(GuildInvitePacket::build(name));
}

void GuildGateway::sendRemove(const std::string& name) {
    sender_.send(GuildRemovePacket::build(name));
}

void GuildGateway::sendDisband() {
    sender_.send(GuildDisbandPacket::build());
}

void GuildGateway::sendLeader(const std::string& name) {
    sender_.send(GuildLeaderPacket::build(name));
}

void GuildGateway::sendPublicNote(const std::string& name, const std::string& note) {
    sender_.send(GuildSetPublicNotePacket::build(name, note));
}

void GuildGateway::sendOfficerNote(const std::string& name, const std::string& note) {
    sender_.send(GuildSetOfficerNotePacket::build(name, note));
}

void GuildGateway::sendAcceptInvite() {
    sender_.send(GuildAcceptPacket::build());
}

void GuildGateway::sendDeclineInvite() {
    sender_.send(GuildDeclineInvitationPacket::build());
}

void GuildGateway::sendCreate(const std::string& guildName) {
    sender_.send(GuildCreatePacket::build(guildName));
}

void GuildGateway::sendAddRank(const std::string& rankName) {
    sender_.send(GuildAddRankPacket::build(rankName));
}

// Removes the lowest rank, which is what the server does with this - it takes
// no index, so there is nothing to get wrong about which one goes.
void GuildGateway::sendDeleteRank() {
    sender_.send(GuildDelRankPacket::build());
}

// CMSG_GUILD_RANK rewrites the rank whole: id, rights, name, gold per day, then
// six pairs of bank tab rights and slots. Everything it carries has to be sent,
// which is why the draft starts from the rank's current values rather than
// from nothing - a field left at zero here is not "unchanged", it is revoked.
void GuildGateway::sendRank(uint32_t rankId, const std::string& rankName,
                            const guild::RankDraft& rank) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_RANK));
    packet.writeUInt32(rankId);
    packet.writeUInt32(rank.rights);
    packet.writeString(rankName);
    packet.writeUInt32(rank.goldLimit);
    for (size_t t = 0; t < 6; ++t) {
        packet.writeUInt32(rank.tabRights[t]);
        packet.writeUInt32(rank.tabSlots[t]);
    }
    sender_.send(packet);
}

uint64_t GuildGateway::playerGuid() const {
    return world_.characterGuid();
}

uint32_t GuildGateway::ownGuildId() const {
    // The player's own field first: it is what the server keeps current, and
    // the character list's copy was taken at login - before any guild joined
    // or left since.
    if (const uint32_t live = playerGuildId(world_, world_.characterGuid())) return live;
    const Character* ch = world_.activeCharacter();
    return ch ? ch->guildId : 0;
}

uint32_t playerGuildId(const SocialQueries& world, uint64_t guid) {
    auto entity = world.entities().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) return 0;
    const uint16_t ufUnitEnd = fieldIndex(UF::UNIT_END);
    if (ufUnitEnd == 0xFFFF) return 0;
    return entity->getField(ufUnitEnd + 3);
}

std::unordered_map<uint32_t, std::string> GuildNameFile::load() const {
    std::unordered_map<uint32_t, std::string> names;
    std::ifstream in(guildNameCachePath());
    if (!in.is_open()) return names;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.find('\t');
        if (separator == std::string::npos) continue;
        try {
            const auto guildId = static_cast<uint32_t>(std::stoul(line.substr(0, separator)));
            std::string guildName = line.substr(separator + 1);
            if (guildId != 0 && !guildName.empty()) names[guildId] = std::move(guildName);
        } catch (...) {
            // Ignore stale or hand-edited cache lines.
        }
    }
    return names;
}

void GuildNameFile::save(const std::unordered_map<uint32_t, std::string>& names) {
    const std::filesystem::path path = guildNameCachePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save guild name cache to ", path.string());
        return;
    }
    for (const auto& [guildId, guildName] : names) {
        if (guildId != 0 && !guildName.empty()) out << guildId << '\t' << guildName << '\n';
    }
}

// Layout verified against AzerothCore's GuildEventLogQueryResults::Write
// (GuildPackets.cpp:146): uint8 count, then per entry a type byte, the acting
// player's guid, a second guid only when the type is neither join nor leave, a
// rank byte only for promote and demote, and a date that is seconds *ago*.
// Two of the five fields are conditional, which is the whole difficulty: a
// reader that always takes the second guid loses eight bytes on every join and
// leave, and those are most of a guild log.
std::optional<std::vector<guild::EventLogEntry>> readGuildEventLog(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 1) return std::nullopt;
    const uint8_t count = packet.readUInt8();
    std::vector<guild::EventLogEntry> entries;
    entries.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (rem() < 9) break;                 // type + one guid at minimum
        guild::EventLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        // Everything but join (2) and leave (6) names a second player.
        if (e.type != 2 && e.type != 6) {
            if (rem() < 8) break;
            e.otherGuid = packet.readUInt64();
        }
        // Promote (3) and demote (4) carry the new rank.
        if (e.type == 3 || e.type == 4) {
            if (rem() < 1) break;
            e.newRank = packet.readUInt8();
        }
        if (rem() < 4) break;
        e.secondsAgo = packet.readUInt32();
        entries.push_back(e);
    }
    LOG_INFO("Guild event log: ", entries.size(), " entries");
    return entries;
}

guild::ServerEvent toServerEvent(const GuildEventData& data) {
    guild::ServerEvent e;
    e.type = data.eventType;
    e.numStrings = data.numStrings;
    for (int i = 0; i < 3; ++i) e.strings[i] = data.strings[i];
    return e;
}

std::vector<core::EventBus::Subscription>
subscribeGuildNotices(core::EventBus& events, ChatLog& chat, FrameXmlEvents& frameXml,
                      SocialQueries& world, const core::CVarStore& cvars) {
    using namespace guild;
    std::vector<core::EventBus::Subscription> s;
    auto line = [&chat](const std::string& text) { chat.addSystemChatMessage(text); };
    // What the game prints for a member's doing, in its own words (ERR_GUILD_*
    // and the friend list's online and offline lines), as a system line. These
    // went out as guild chat with no sender, and the interface puts the
    // player's own name on a local line without one: "[Guild] [You]: [Guild]
    // Coggs has come online."
    auto memberLine = [&world](const std::string& text) {
        MessageChatData msg;
        msg.type = ChatType::SYSTEM;
        msg.language = ChatLanguage::UNIVERSAL;
        msg.message = text;
        world.addLocalChatMessage(msg);
    };
    // Guild Member Notify, offered by the interface panel. The online state is
    // still tracked when the line is not wanted - the roster's idea of who is
    // online is not the player's idea of what is worth reading.
    auto memberNotify = [&cvars]() { return cvars.get("guildMemberNotify", "1") != "0"; };
    auto error = [&events, &frameXml](const std::string& text) { raiseUiError(events, frameXml, text); };
    auto addon = [&frameXml](const char* name, std::vector<std::string> args = {}) {
        if (frameXml.frameXmlListening()) frameXml.fireFrameXmlEvent(name, args);
    };

    // What friendsframe and the paperdoll re-check IsInGuild on.
    s.push_back(events.subscribe<MembershipChanged>([addon](const auto&) { addon("PLAYER_GUILD_UPDATE"); }));
    // The argument is "you may request a roster", and every reader answers it
    // with `if arg1 then GuildRoster() end` - friendsframe, the guild bank and
    // the calendar all do. A true after a fresh roster would have each of them
    // ask for one, whose reply would fire this again, forever.
    s.push_back(events.subscribe<RosterChanged>([addon](const RosterChanged& e) {
        addon("GUILD_ROSTER_UPDATE", {eventBool(e.mayRequest)});
    }));
    s.push_back(events.subscribe<GuildNamed>([line](const GuildNamed& e) { line("Guild: <" + e.name + ">"); }));
    // SMSG_GUILD_INFO comes with every roster sync; the guild says only a
    // change the player can see, so the banner does not repeat every tick.
    s.push_back(events.subscribe<SummaryChanged>([line](const SummaryChanged& e) {
        line("Guild: " + e.name + " (" + std::to_string(e.members) + " members, " +
             std::to_string(e.accounts) + " accounts)");
    }));

    s.push_back(events.subscribe<MemberPromoted>([memberLine](const MemberPromoted& e) {
        memberLine(e.by + " has promoted " + e.member + " to " + e.rank + ".");
    }));
    s.push_back(events.subscribe<MemberDemoted>([memberLine](const MemberDemoted& e) {
        memberLine(e.by + " has demoted " + e.member + " to " + e.rank + ".");
    }));
    // Not a line from here: chatframe.lua formats GUILD_MOTD_TEMPLATE from the
    // event.
    s.push_back(events.subscribe<MotdChanged>([addon](const MotdChanged& e) { addon("GUILD_MOTD", {e.text}); }));
    s.push_back(events.subscribe<MemberJoined>([memberLine](const MemberJoined& e) {
        memberLine(e.name + " has joined the guild.");
    }));
    s.push_back(events.subscribe<MemberLeft>([memberLine](const MemberLeft& e) {
        memberLine(e.name + " has left the guild.");
    }));
    // The member removed, then who removed them - the order the server sends
    // and ERR_GUILD_REMOVE_SS reads them in.
    s.push_back(events.subscribe<MemberRemoved>([memberLine](const MemberRemoved& e) {
        memberLine(e.member + " has been kicked out of the guild by " + e.by + ".");
    }));
    s.push_back(events.subscribe<LeaderIs>([memberLine](const LeaderIs& e) {
        memberLine(e.name + " is the leader of your guild.");
    }));
    s.push_back(events.subscribe<LeaderChanged>([memberLine](const LeaderChanged& e) {
        memberLine(e.from + " has made " + e.to + " the new Guild Master.");
    }));
    s.push_back(events.subscribe<GuildDisbanded>([memberLine](const auto&) { memberLine("Guild has been disbanded."); }));
    s.push_back(events.subscribe<MemberCameOnline>([memberLine, memberNotify](const MemberCameOnline& e) {
        if (memberNotify()) memberLine("|Hplayer:" + e.name + "|h[" + e.name + "]|h has come online.");
    }));
    s.push_back(events.subscribe<MemberWentOffline>([memberLine, memberNotify](const MemberWentOffline& e) {
        if (memberNotify()) memberLine(e.name + " has gone offline.");
    }));

    // The bank's half of the event list. The server broadcasts these to every
    // member whenever anyone touches the bank, so they are frequent, and not
    // one of them is a chat line - they carry the figures the bank panel reads
    // back through its own API. GuildBankFrame_OnEvent returns early unless the
    // frame is visible, so firing them while it is shut costs nothing.
    s.push_back(events.subscribe<BankMoneyChanged>([&world](const BankMoneyChanged& e) {
        if (auto* bank = world.guildBankFeed()) bank->onMoney(e.copper);
    }));
    s.push_back(events.subscribe<TabardChanged>([addon](const auto&) { addon("GUILDTABARD_UPDATE"); }));
    s.push_back(events.subscribe<BankBagSlotsChanged>([addon](const auto&) { addon("GUILDBANKBAGSLOTS_CHANGED"); }));
    s.push_back(events.subscribe<BankTabsChanged>([addon](const auto&) { addon("GUILDBANK_UPDATE_TABS"); }));
    // The panel answers by querying the tab's new text.
    s.push_back(events.subscribe<BankTextChanged>([addon](const BankTextChanged& e) {
        addon("GUILDBANK_TEXT_CHANGED", {std::to_string(e.tab)});
    }));
    s.push_back(events.subscribe<UnknownServerEvent>([](const UnknownServerEvent& e) {
        // Not a chat line either: an event this client does not know is a gap
        // in it, and guild chat is not where that belongs. Once per type -
        // these arrive as often as the bank ones do.
        static std::array<bool, 256> warned{};
        if (warned[e.type]) return;
        warned[e.type] = true;
        LOG_WARNING("SMSG_GUILD_EVENT: unhandled type ", static_cast<int>(e.type),
                    " strings=", static_cast<int>(e.numStrings),
                    e.numStrings >= 1 ? (" first=" + e.first) : std::string());
    }));

    s.push_back(events.subscribe<InviteReceived>([line, addon](const InviteReceived& e) {
        line(e.inviter + " has invited you to join " + e.guildName + ".");
        addon("GUILD_INVITE_REQUEST", {e.inviter, e.guildName});
    }));
    s.push_back(events.subscribe<InviteDeclined>([line](const InviteDeclined& e) {
        line(e.name + " declined your guild invitation.");
    }));
    s.push_back(events.subscribe<GuildCreated>([line](const auto&) { line("Guild created."); }));
    s.push_back(events.subscribe<GuildInviteSent>([line](const GuildInviteSent& e) {
        line("You have invited " + e.name + " to the guild.");
    }));
    s.push_back(events.subscribe<LeftGuild>([line](const auto&) { line("You have left the guild."); }));
    s.push_back(events.subscribe<CommandFailed>([line, error](const CommandFailed& e) {
        const std::string msg = commandError(e);
        error(msg);
        line(msg);
    }));
    s.push_back(events.subscribe<EventLogChanged>([addon](const auto&) { addon("GUILD_EVENT_LOG_UPDATE"); }));

    s.push_back(events.subscribe<MotdSent>([line](const auto&) { line("Guild MOTD updated."); }));
    s.push_back(events.subscribe<PromotionRequested>([line](const PromotionRequested& e) {
        line("Promoting " + e.name + "...");
    }));
    s.push_back(events.subscribe<DemotionRequested>([line](const DemotionRequested& e) {
        line("Demoting " + e.name + "...");
    }));
    s.push_back(events.subscribe<LeaveRequested>([line](const auto&) { line("Leaving guild..."); }));
    s.push_back(events.subscribe<InviteRequested>([line](const InviteRequested& e) {
        line("Inviting " + e.name + " to guild...");
    }));
    s.push_back(events.subscribe<PlayerNameRequired>([error](const auto&) {
        error("You must specify a player name.");
    }));
    return s;
}

} // namespace wowee::game
