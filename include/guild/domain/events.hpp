#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace wowee::guild {

// What happened to the player's guild. Guild returns these; the service
// publishes them, and the chat log, the addon layer and the bank subscribe.

/// Whether the player is in a guild, or which one, changed (PLAYER_GUILD_UPDATE).
struct MembershipChanged {};
/// The roster the client holds changed or went stale (GUILD_ROSTER_UPDATE).
/// `mayRequest` tells the interface it may ask for a fresh roster itself: false
/// when the client is about to ask, or when the roster in hand is fresh.
struct RosterChanged { bool mayRequest = false; };
/// The client should ask the server for a fresh roster.
struct RosterOutdated {};
/// A guild's name became known; worth remembering across sessions.
struct GuildNameLearned { uint32_t guildId = 0; std::string name; };
/// The player's own guild's name became known.
struct GuildNamed { std::string name; };
/// The guild's headline figures changed.
struct SummaryChanged { std::string name; uint32_t members = 0; uint32_t accounts = 0; };

// Things members did, as SMSG_GUILD_EVENT reports them.
struct MemberPromoted  { std::string by; std::string member; std::string rank; };
struct MemberDemoted   { std::string by; std::string member; std::string rank; };
/// Empty when the event carried no text at all.
struct MotdChanged     { std::string text; };
struct MemberJoined    { std::string name; };
struct MemberLeft      { std::string name; };
struct MemberRemoved   { std::string member; std::string by; };
struct LeaderIs        { std::string name; };
struct LeaderChanged   { std::string from; std::string to; };
struct GuildDisbanded {};
/// Only when the roster said otherwise: the login flood of sign-ons for
/// members already listed as online is not news.
struct MemberCameOnline  { std::string name; };
struct MemberWentOffline { std::string name; };
struct TabardChanged {};
struct BankMoneyChanged  { uint64_t copper = 0; };
struct BankBagSlotsChanged {};
struct BankTabsChanged {};
/// `tab` counts from one, as the interface does.
struct BankTextChanged   { int tab = 0; };
/// A type this client does not know.
struct UnknownServerEvent { uint8_t type = 0; uint8_t numStrings = 0; std::string first; };

struct InviteReceived  { std::string inviter; std::string guildName; };
struct InviteDeclined  { std::string name; };

// The server's answer to a guild command.
struct GuildCreated {};
struct GuildInviteSent { std::string name; };
struct LeftGuild {};
struct CommandFailed   { uint32_t errorCode = 0; std::string name; };

struct EventLogChanged {};

// What became of a request the player made, published by the service so the
// chat log reports it whichever way the request was made.
struct MotdSent {};
struct PromotionRequested { std::string name; };
struct DemotionRequested  { std::string name; };
struct LeaveRequested {};
struct InviteRequested    { std::string name; };
/// Refused: the request needs a player's name and had none.
struct PlayerNameRequired {};

using GuildChange = std::variant<
    MembershipChanged, RosterChanged, RosterOutdated, GuildNameLearned, GuildNamed,
    SummaryChanged, MemberPromoted, MemberDemoted, MotdChanged, MemberJoined,
    MemberLeft, MemberRemoved, LeaderIs, LeaderChanged, GuildDisbanded,
    MemberCameOnline, MemberWentOffline, TabardChanged, BankMoneyChanged,
    BankBagSlotsChanged, BankTabsChanged, BankTextChanged, UnknownServerEvent,
    InviteReceived, InviteDeclined, GuildCreated, GuildInviteSent, LeftGuild,
    CommandFailed, EventLogChanged>;

} // namespace wowee::guild
