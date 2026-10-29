// The guild, end to end: the server's guild packets, what the interface reads
// and is told, the lines the player sees, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "guild/domain/records.hpp"
#include "guild/services/guild.hpp"
#include "guild_bank/domain/records.hpp"
#include "guild_bank/services/guild_bank.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint32_t kGuildId = 7;
constexpr uint64_t kGarrosh = 0x31;
constexpr uint64_t kJaina = 0x32;

struct Member {
    uint64_t guid;
    std::string name;
    bool online;
    uint32_t rank;
};

/// SMSG_GUILD_ROSTER as 3.3.5a writes it: the count, the MOTD and info text,
/// the ranks with their gold limit and six bank tabs, then the members.
std::vector<uint8_t> roster(const std::vector<Member>& members) {
    Body b;
    b.u32(static_cast<uint32_t>(members.size())).cstr("Raid at eight").cstr("Be nice");
    b.u32(2);                                           // ranks
    for (uint32_t r = 0; r < 2; ++r) {
        b.u32(r == 0 ? 0xF1FFFu : 0x43u);               // rights
        b.u32(r == 0 ? 0u : 250u);                      // gold per day
        for (uint32_t t = 0; t < 6; ++t) b.u32(0x10 + t).u32(0x20 + t);
    }
    for (const auto& m : members) {
        b.u64(m.guid).u8(m.online ? 1 : 0).cstr(m.name).u32(m.rank).u8(80).u8(1).u8(0).u32(1519);
        if (!m.online) b.f32(1.5f);
        b.cstr("").cstr("");
    }
    return b.bytes();
}

/// SMSG_GUILD_QUERY_RESPONSE: the id, the name, ten rank names, the tabard
/// and the rank count.
std::vector<uint8_t> identity(uint32_t guildId, const std::string& name) {
    Body b;
    b.u32(guildId).cstr(name).cstr("Warchief").cstr("Grunt");
    for (int i = 2; i < 10; ++i) b.cstr("");
    b.u32(1).u32(2).u32(3).u32(4).u32(5).u32(2);
    return b.bytes();
}

std::vector<uint8_t> event(uint8_t type, const std::vector<std::string>& strings) {
    Body b;
    b.u8(type).u8(static_cast<uint8_t>(strings.size()));
    for (const auto& s : strings) b.cstr(s);
    return b.bytes();
}

/// The player's own PLAYER_GUILDID, as the server keeps it current.
void inGuildField(GameHarness& h, uint32_t guildId) {
    h.playerInView();
    const auto field = static_cast<uint16_t>(wowee::game::fieldIndex(wowee::game::UF::UNIT_END) + 3);
    h.receive(Opcode::SMSG_UPDATE_OBJECT, GameHarness::valuesBody(h.player().guid, {{field, guildId}}));
    h.pump();
}

bool chatSaid(GameHarness& h, const std::string& text) {
    const auto& history = h.game().chat().getChatHistory();
    return std::any_of(history.begin(), history.end(),
                       [&](const auto& line) { return line.message == text; });
}

std::vector<std::string> uiErrors(GameHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.addonEvents("UI_ERROR_MESSAGE")) out.push_back(e.args.at(0));
    return out;
}

}  // namespace

TEST_CASE("a roster makes the player a member, and the guild is asked its name", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    inGuildField(h, kGuildId);
    h.clearRecords();

    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{h.player().guid, "Harness", true, 1}, {kGarrosh, "Garrosh", true, 0}}));
    h.pump();

    REQUIRE(h.guild().isInGuild());
    REQUIRE(h.guild().hasGuildRoster());
    CHECK(h.guild().getGuildRoster().members.size() == 2);
    CHECK(h.guild().getPlayerGuildRankIndex() == 1);
    CHECK(h.guild().getPlayerGuildRankRights() == 0x43u);
    CHECK(h.addonEvents("PLAYER_GUILD_UPDATE").size() == 1);
    // A fresh roster: nobody is to ask for another.
    const auto rosterUpdates = h.addonEvents("GUILD_ROSTER_UPDATE");
    REQUIRE(rosterUpdates.size() == 1);
    CHECK(rosterUpdates[0].args.at(0) != "1");
    auto* query = h.lastSent(Opcode::CMSG_GUILD_QUERY);
    REQUIRE(query);
    CHECK(query->readUInt32() == kGuildId);

    h.receive(Opcode::SMSG_GUILD_QUERY_RESPONSE, identity(kGuildId, "Horde Inc"));
    h.pump();
    CHECK(h.guild().getGuildName() == "Horde Inc");
    CHECK(h.guild().getGuildRankNames().at(1) == "Grunt");
    CHECK(chatSaid(h, "Guild: <Horde Inc>"));

    // A second roster is not a second joining.
    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{h.player().guid, "Harness", true, 1}}));
    h.pump();
    CHECK(h.addonEvents("PLAYER_GUILD_UPDATE").size() == 2);   // the roster, then the name
}

TEST_CASE("another guild's answer does not name the player's", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    inGuildField(h, kGuildId);

    h.receive(Opcode::SMSG_GUILD_QUERY_RESPONSE, identity(99, "Alliance Ltd"));
    h.pump();

    CHECK(h.guild().getGuildName().empty());
    // ...though its name is remembered for nameplates, without asking.
    h.clearRecords();
    CHECK(h.guild().lookupGuildName(99) == "Alliance Ltd");
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_QUERY));
}

TEST_CASE("an unknown guild's name is asked for once and read when it comes", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    h.clearRecords();

    CHECK(h.guild().lookupGuildName(42).empty());
    CHECK(h.guild().lookupGuildName(42).empty());
    CHECK(h.countSent(Opcode::CMSG_GUILD_QUERY) == 1);

    h.receive(Opcode::SMSG_GUILD_QUERY_RESPONSE, identity(42, "Ironforge Traders"));
    h.pump();
    CHECK(h.guild().lookupGuildName(42) == "Ironforge Traders");
}

TEST_CASE("the character list asks only for the guild names it does not know", "[guild][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    alice.guildId = kGuildId;
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";
    bob.guildId = 8;

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_GUILD_QUERY_RESPONSE, identity(kGuildId, "Horde Inc"));
    h.pump();
    h.game().requestLogout(false);
    h.receive(Opcode::SMSG_LOGOUT_COMPLETE);
    h.pump();
    h.game().returnToCharacterSelect();
    h.clearRecords();

    h.receive(Opcode::SMSG_CHAR_ENUM, GameHarness::charEnumBody({alice, bob}));
    h.pump();

    REQUIRE(h.countSent(Opcode::CMSG_GUILD_QUERY) == 1);
    CHECK(h.lastSent(Opcode::CMSG_GUILD_QUERY)->readUInt32() == 8);
}

TEST_CASE("the guild's figures are said when they change", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    inGuildField(h, kGuildId);

    // SMSG_GUILD_INFO: the name, the creation date, the members and accounts.
    auto info = [](uint32_t members) { return Body().cstr("Horde Inc").u32(0).u32(members).u32(12).bytes(); };
    h.receive(Opcode::SMSG_GUILD_INFO, info(40));
    h.receive(Opcode::SMSG_GUILD_INFO, info(40));
    h.pump();

    const auto& history = h.game().chat().getChatHistory();
    CHECK(std::count_if(history.begin(), history.end(), [](const auto& line) {
              return line.message == "Guild: Horde Inc (40 members, 12 accounts)";
          }) == 1);
    CHECK(h.guild().getGuildInfoData().numMembers == 40);
    // Named by the player's live guild id: a guild joined this session too.
    CHECK(h.guild().getGuildName() == "Horde Inc");
    CHECK(h.guild().lookupGuildName(kGuildId) == "Horde Inc");
}

TEST_CASE("what a member did is a system line, and the client asks for the roster it changed",
          "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{h.player().guid, "Harness", true, 1}, {kGarrosh, "Garrosh", true, 0}}));
    h.pump();
    h.clearRecords();

    h.receive(Opcode::SMSG_GUILD_EVENT, event(0 /* promotion */, {"Garrosh", "Jaina", "Officer"}));
    h.pump();

    CHECK(chatSaid(h, "Garrosh has promoted Jaina to Officer."));
    CHECK(h.addonEvents("CHAT_MSG_GUILD").empty());
    // The client asks itself, so the interface is told not to.
    const auto updates = h.addonEvents("GUILD_ROSTER_UPDATE");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].args.at(0) != "1");
    CHECK(h.countSent(Opcode::CMSG_GUILD_ROSTER) == 1);

    h.receive(Opcode::SMSG_GUILD_EVENT, event(5 /* removed */, {"Jaina", "Garrosh"}));
    h.receive(Opcode::SMSG_GUILD_EVENT, event(7 /* leader changed */, {"Garrosh", "Harness"}));
    h.pump();
    CHECK(chatSaid(h, "Jaina has been kicked out of the guild by Garrosh."));
    CHECK(chatSaid(h, "Garrosh has made Harness the new Guild Master."));
}

TEST_CASE("without a roster in hand the interface is the one to ask", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GUILD_EVENT, event(3 /* joined */, {"Jaina"}));
    h.pump();

    CHECK(chatSaid(h, "Jaina has joined the guild."));
    const auto updates = h.addonEvents("GUILD_ROSTER_UPDATE");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].args.at(0) == "1");
    CHECK(h.countSent(Opcode::CMSG_GUILD_ROSTER) == 0);
}

TEST_CASE("a member coming online is said once, and not at all when the player does not want it",
          "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GUILD_ROSTER,
              roster({{h.player().guid, "Harness", true, 1}, {kGarrosh, "Garrosh", true, 0}, {kJaina, "Jaina", false, 1}}));
    h.pump();

    // The login flood: Garrosh is already listed online.
    h.receive(Opcode::SMSG_GUILD_EVENT, event(12 /* signed on */, {"Garrosh"}));
    h.receive(Opcode::SMSG_GUILD_EVENT, event(12, {"Jaina"}));
    h.pump();
    CHECK_FALSE(chatSaid(h, "|Hplayer:Garrosh|h[Garrosh]|h has come online."));
    CHECK(chatSaid(h, "|Hplayer:Jaina|h[Jaina]|h has come online."));

    h.cvars().set("guildMemberNotify", "0");
    h.receive(Opcode::SMSG_GUILD_EVENT, event(13 /* signed off */, {"Jaina"}));
    h.pump();
    CHECK_FALSE(chatSaid(h, "Jaina has gone offline."));
    // Still tracked: the roster says she is gone.
    const auto& members = h.guild().getGuildRoster().members;
    const auto jaina = std::find_if(members.begin(), members.end(), [](const auto& m) { return m.name == "Jaina"; });
    REQUIRE(jaina != members.end());
    CHECK_FALSE(jaina->online);
}

TEST_CASE("the message of the day goes to the interface, not into chat from here", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    const size_t linesBefore = h.game().chat().getChatHistory().size();

    h.receive(Opcode::SMSG_GUILD_EVENT, event(2 /* motd */, {"Raid at eight"}));
    h.pump();

    const auto motd = h.addonEvents("GUILD_MOTD");
    REQUIRE(motd.size() == 1);
    CHECK(motd[0].args == std::vector<std::string>{"Raid at eight"});
    CHECK(h.game().chat().getChatHistory().size() == linesBefore);
}

TEST_CASE("the guild bank's balance event is the bank's figure, not a chat line", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    const size_t linesBefore = h.game().chat().getChatHistory().size();

    // Type 17, one string: the balance as sixteen zero-padded hex digits,
    // broadcast to every member whenever anyone touches the bank. Unhandled,
    // it reached guild chat as "Guild event 17: 000000000051451B".
    h.receive(Opcode::SMSG_GUILD_EVENT, event(17, {"000000000051451B"}));
    h.pump();

    CHECK(h.game().chat().getChatHistory().size() == linesBefore);
    CHECK(h.addonEvents("CHAT_MSG_SYSTEM").empty());
    CHECK(h.addonEvents("CHAT_MSG_GUILD").empty());
    // 532g 61s 7c, read as hex.
    CHECK(h.guildBank().getGuildBankData().money == 5326107u);

    // The tab's text changed: the panel counts tabs from one.
    h.receive(Opcode::SMSG_GUILD_EVENT, event(19, {"2"}));
    h.pump();
    const auto text = h.addonEvents("GUILDBANK_TEXT_CHANGED");
    REQUIRE(text.size() == 1);
    CHECK(text[0].args == std::vector<std::string>{"3"});
}

TEST_CASE("a new tabard asks the guild again for its emblem", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    inGuildField(h, kGuildId);
    h.clearRecords();

    h.receive(Opcode::SMSG_GUILD_EVENT, event(9 /* tabard */, {}));
    h.pump();

    CHECK(h.addonEvents("GUILDTABARD_UPDATE").size() == 1);
    auto* query = h.lastSent(Opcode::CMSG_GUILD_QUERY);
    REQUIRE(query);
    CHECK(query->readUInt32() == kGuildId);
}

TEST_CASE("a disbanded guild is no longer the player's, and the interface hears it first",
          "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{h.player().guid, "Harness", true, 0}}));
    h.receive(Opcode::SMSG_GUILD_QUERY_RESPONSE, identity(kGuildId, "Horde Inc"));
    h.pump();
    REQUIRE(h.guild().isInGuild());
    h.clearRecords();

    h.receive(Opcode::SMSG_GUILD_EVENT, event(8 /* disbanded */, {}));
    h.pump();

    CHECK_FALSE(h.guild().isInGuild());
    CHECK_FALSE(h.guild().hasGuildRoster());
    CHECK(h.guild().getGuildName().empty());
    CHECK(chatSaid(h, "Guild has been disbanded."));
    // PLAYER_GUILD_UPDATE, then the line, then the roster event.
    const size_t membership = h.firedAt("PLAYER_GUILD_UPDATE");
    const size_t line = h.firedAt("CHAT_MSG_SYSTEM", "Guild has been disbanded.");
    const size_t rosterUpdate = h.firedAt("GUILD_ROSTER_UPDATE");
    REQUIRE(membership < h.addonEvents().size());
    CHECK(membership < line);
    CHECK(line < rosterUpdate);
}

TEST_CASE("leaving the guild ends membership", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{h.player().guid, "Harness", true, 1}}));
    h.pump();

    h.guild().leaveGuild();
    h.pump();
    CHECK(h.lastSent(Opcode::CMSG_GUILD_LEAVE));
    CHECK(chatSaid(h, "Leaving guild..."));

    // SMSG_GUILD_COMMAND_RESULT: quit, no name, no error.
    h.receive(Opcode::SMSG_GUILD_COMMAND_RESULT, Body().u32(2).cstr("").u32(0).bytes());
    h.pump();
    CHECK(chatSaid(h, "You have left the guild."));
    CHECK_FALSE(h.guild().isInGuild());
    CHECK(h.addonEvents("PLAYER_GUILD_UPDATE").size() == 2);
}

TEST_CASE("a refused guild command says why on the error line and in chat", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GUILD_COMMAND_RESULT, Body().u32(1 /* invite */).cstr("Jaina").u32(4).bytes());
    h.receive(Opcode::SMSG_GUILD_COMMAND_RESULT, Body().u32(1).cstr("").u32(14).bytes());
    h.receive(Opcode::SMSG_GUILD_COMMAND_RESULT, Body().u32(1).cstr("Jaina").u32(99).bytes());
    h.pump();

    const std::vector<std::string> said{"No player named \"Jaina\" is online.",
                                        "\"that player\" has already been invited to a guild.",
                                        "Guild command failed for Jaina (error 99)"};
    CHECK(uiErrors(h) == said);
    for (const auto& s : said) CHECK(chatSaid(h, s));
}

TEST_CASE("an invitation is offered, and accepting it joins under the invite's name", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GUILD_INVITE, Body().cstr("Garrosh").cstr("Horde Inc").bytes());
    h.pump();
    const auto offered = h.addonEvents("GUILD_INVITE_REQUEST");
    REQUIRE(offered.size() == 1);
    CHECK(offered[0].args == std::vector<std::string>{"Garrosh", "Horde Inc"});
    CHECK(chatSaid(h, "Garrosh has invited you to join Horde Inc."));
    REQUIRE(h.guild().hasPendingGuildInvite());

    h.guild().acceptGuildInvite();
    CHECK(h.lastSent(Opcode::CMSG_GUILD_ACCEPT));
    // The server sends the join to the guild, not a roster to the joiner.
    CHECK(h.lastSent(Opcode::CMSG_GUILD_ROSTER));
    CHECK_FALSE(h.guild().hasPendingGuildInvite());

    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{h.player().guid, "Harness", true, 1}}));
    h.pump();
    CHECK(h.guild().isInGuild());
    CHECK(h.guild().getGuildName() == "Horde Inc");
}

TEST_CASE("a declined invitation is said to the inviter", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_GUILD_DECLINE, Body().cstr("Jaina").bytes());
    h.pump();

    CHECK(chatSaid(h, "Jaina declined your guild invitation."));
}

TEST_CASE("a request that names nobody is refused, one that does goes out", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();

    h.guild().promoteGuildMember("");
    h.guild().inviteToGuild("");
    h.pump();
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_PROMOTE));
    CHECK_FALSE(h.lastSent(Opcode::CMSG_GUILD_INVITE));
    CHECK(uiErrors(h) == std::vector<std::string>{"You must specify a player name.", "You must specify a player name."});

    h.guild().promoteGuildMember("Jaina");
    auto* promote = h.lastSent(Opcode::CMSG_GUILD_PROMOTE);
    REQUIRE(promote);
    CHECK(promote->readString() == "Jaina");
    CHECK(chatSaid(h, "Promoting Jaina..."));

    h.guild().setGuildMotd("Raid at nine");
    auto* motd = h.lastSent(Opcode::CMSG_GUILD_MOTD);
    REQUIRE(motd);
    CHECK(motd->readString() == "Raid at nine");
    CHECK(chatSaid(h, "Guild MOTD updated."));
}

TEST_CASE("a rank is saved whole: what the panel did not touch goes back as it was", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{h.player().guid, "Harness", true, 0}}));
    h.pump();

    // The panel counts ranks from one.
    h.guild().setSelectedGuildRank(2);
    h.guild().pendingGuildRankRef().goldLimit = 500;
    h.guild().saveGuildRank("Grunt");

    auto* rank = h.lastSent(Opcode::CMSG_GUILD_RANK);
    REQUIRE(rank);
    CHECK(rank->readUInt32() == 1);             // zero-based on the wire
    CHECK(rank->readUInt32() == 0x43u);         // the rank's rights, untouched
    CHECK(rank->readString() == "Grunt");
    CHECK(rank->readUInt32() == 500);
    CHECK(rank->readUInt32() == 0x10);          // first tab's rights, as the roster had them
    CHECK(rank->readUInt32() == 0x20);
    CHECK(h.lastSent(Opcode::CMSG_GUILD_ROSTER));
}

TEST_CASE("the guild's event log reads joins without a second player", "[guild][harness]") {
    GameHarness h;
    h.enterWorld();

    h.guild().requestGuildEventLog();
    CHECK(h.lastSent(Opcode::MSG_GUILD_EVENT_LOG_QUERY));

    // A join (type 2): the player and the date. A promotion (type 3): the
    // player, who promoted them, the new rank and the date.
    h.receive(Opcode::MSG_GUILD_EVENT_LOG_QUERY,
              Body().u8(2).u8(2).u64(kJaina).u32(3600).u8(3).u64(kGarrosh).u64(kJaina).u8(1).u32(60).bytes());
    h.pump();

    const auto& log = h.guild().getGuildEventLog();
    REQUIRE(log.size() == 2);
    CHECK(log[0].playerGuid == kJaina);
    CHECK(log[0].otherGuid == 0);
    CHECK(log[0].secondsAgo == 3600);
    CHECK(log[1].otherGuid == kJaina);
    CHECK(log[1].newRank == 1);
    CHECK(log[1].secondsAgo == 60);
    CHECK(h.addonEvents("GUILD_EVENT_LOG_UPDATE").size() == 1);
}

TEST_CASE("the next character has none of the last one's guild", "[guild][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_GUILD_ROSTER, roster({{alice.guid, "Alice", true, 0}}));
    h.receive(Opcode::SMSG_GUILD_INVITE, Body().cstr("Garrosh").cstr("Horde Inc").bytes());
    h.receive(Opcode::MSG_GUILD_EVENT_LOG_QUERY, Body().u8(1).u8(2).u64(kJaina).u32(60).bytes());
    h.pump();
    REQUIRE(h.guild().isInGuild());
    REQUIRE(h.guild().hasPendingGuildInvite());

    h.relogAs(bob);

    CHECK_FALSE(h.guild().isInGuild());
    CHECK_FALSE(h.guild().hasGuildRoster());
    CHECK_FALSE(h.guild().hasPendingGuildInvite());
    CHECK(h.guild().getGuildEventLog().empty());
}
