// The guild's rules, with nothing but the domain model linked.
#include <catch_amalgamated.hpp>

#include "guild/domain/guild.hpp"
#include "guild/domain/guild_names.hpp"
#include "guild_event_fixture.hpp"

#include <algorithm>
#include <variant>

using namespace wowee::guild;

namespace {

template <typename E>
bool has(const std::vector<GuildChange>& changes) {
    return std::any_of(changes.begin(), changes.end(),
                       [](const GuildChange& c) { return std::holds_alternative<E>(c); });
}

template <typename E>
const E& first(const std::vector<GuildChange>& changes) {
    for (const auto& c : changes)
        if (auto* p = std::get_if<E>(&c)) return *p;
    FAIL("change not reported");
    throw;
}

Roster rosterWith(std::vector<RosterMember> members, std::vector<Rank> ranks = {}) {
    Roster r;
    r.members = std::move(members);
    r.ranks = std::move(ranks);
    return r;
}

RosterMember member(const std::string& name, bool online, uint64_t guid = 0, uint32_t rank = 0) {
    RosterMember m;
    m.name = name;
    m.online = online;
    m.guid = guid;
    m.rankIndex = rank;
    return m;
}

} // namespace

TEST_CASE("The first roster makes the player a member; the roster in hand is fresh", "[guild]") {
    Guild g;
    auto first_ = g.rosterReceived(rosterWith({}));
    CHECK(has<MembershipChanged>(first_));
    CHECK(first<RosterChanged>(first_).mayRequest == false);
    CHECK(g.isMember(0));
    CHECK_FALSE(has<MembershipChanged>(g.rosterReceived(rosterWith({}))));
}

TEST_CASE("A joined guild is called what the invite called it until it names itself", "[guild]") {
    Guild g;
    g.inviteReceived("Thrall", "Horde Inc");
    CHECK(g.hasPendingInvite());
    g.inviteAccepted();
    CHECK_FALSE(g.hasPendingInvite());
    g.rosterReceived(rosterWith({}));
    CHECK(g.name() == "Horde Inc");
}

TEST_CASE("Another guild's identity teaches its name and nothing else", "[guild]") {
    Guild g;
    Identity other;
    other.guildId = 5;
    other.guildName = "Alliance Ltd";
    auto changes = g.identityReceived(other, 7);
    CHECK(first<GuildNameLearned>(changes).guildId == 5);
    CHECK(g.name().empty());

    Identity own;
    own.guildId = 7;
    own.guildName = "Horde Inc";
    own.rankNames[0] = "Warchief";
    auto mine = g.identityReceived(own, 7);
    CHECK(g.name() == "Horde Inc");
    CHECK(g.rankNames().at(0) == "Warchief");
    CHECK(first<GuildNamed>(mine).name == "Horde Inc");
    CHECK(has<MembershipChanged>(mine));
}

TEST_CASE("A sign-on for a member already listed online is not announced", "[guild]") {
    Guild g;
    g.rosterReceived(rosterWith({member("Rexxar", true), member("Jaina", false)}));
    CHECK_FALSE(has<MemberCameOnline>(g.eventReceived(guildEvent(GuildEvent::SIGNED_ON, {"Rexxar"}))));
    CHECK(first<MemberCameOnline>(g.eventReceived(guildEvent(GuildEvent::SIGNED_ON, {"Jaina"}))).name == "Jaina");
    CHECK(has<MemberWentOffline>(g.eventReceived(guildEvent(GuildEvent::SIGNED_OFF, {"Rexxar"}))));
    // Not in the roster yet: announced rather than swallowed.
    CHECK(has<MemberCameOnline>(g.eventReceived(guildEvent(GuildEvent::SIGNED_ON, {"Garrosh"}))));
}

TEST_CASE("After a promotion the client asks for the roster and tells the interface not to", "[guild]") {
    Guild g;
    // Without a roster the client does not ask, so the interface may.
    auto noRoster = g.eventReceived(guildEvent(GuildEvent::PROMOTION, {"Thrall", "Rexxar", "Officer"}));
    CHECK(first<RosterChanged>(noRoster).mayRequest == true);
    CHECK_FALSE(has<RosterOutdated>(noRoster));

    g.rosterReceived(rosterWith({}));
    auto withRoster = g.eventReceived(guildEvent(GuildEvent::PROMOTION, {"Thrall", "Rexxar", "Officer"}));
    const auto& promoted = first<MemberPromoted>(withRoster);
    CHECK(promoted.by == "Thrall");
    CHECK(promoted.member == "Rexxar");
    CHECK(promoted.rank == "Officer");
    CHECK(first<RosterChanged>(withRoster).mayRequest == false);
    CHECK(has<RosterOutdated>(withRoster));
}

TEST_CASE("A sign-on leaves the roster request to the interface", "[guild]") {
    Guild g;
    g.rosterReceived(rosterWith({member("Jaina", false)}));
    auto changes = g.eventReceived(guildEvent(GuildEvent::SIGNED_ON, {"Jaina"}));
    CHECK(first<RosterChanged>(changes).mayRequest == true);
    CHECK_FALSE(has<RosterOutdated>(changes));
}

TEST_CASE("Leaving the guild ends membership", "[guild]") {
    Guild g;
    g.rosterReceived(rosterWith({member("Thrall", true)}));
    auto changes = g.commandResult(2, 0, "");
    CHECK(has<LeftGuild>(changes));
    CHECK(has<MembershipChanged>(changes));
    CHECK_FALSE(g.isMember(0));
    CHECK_FALSE(g.hasRoster());
}

TEST_CASE("A disbanded guild is no longer the player's", "[guild]") {
    Guild g;
    g.rosterReceived(rosterWith({}));
    auto changes = g.eventReceived(guildEvent(GuildEvent::DISBANDED));
    CHECK(has<GuildDisbanded>(changes));
    // Membership first: the interface rechecks IsInGuild before the line.
    REQUIRE(changes.size() >= 2);
    CHECK(std::holds_alternative<MembershipChanged>(changes[0]));
    CHECK_FALSE(g.isMember(0));
}

TEST_CASE("A failed command keeps the guild and reports the code", "[guild]") {
    Guild g;
    g.rosterReceived(rosterWith({}));
    auto changes = g.commandResult(1, 11, "Bob");
    CHECK(first<CommandFailed>(changes).errorCode == 11);
    CHECK(first<CommandFailed>(changes).name == "Bob");
    CHECK(g.isMember(0));
}

TEST_CASE("A bank text change names the tab as the interface counts it", "[guild]") {
    Guild g;
    CHECK(first<BankTextChanged>(g.eventReceived(guildEvent(GuildEvent::BANK_TEXT_CHANGED, {"2"}))).tab == 3);
}

TEST_CASE("The rank editor starts from the selected rank's current values", "[guild]") {
    Guild g;
    Rank master;
    master.rights = 0xFFFF;
    Rank officer;
    officer.rights = 0x3;
    officer.goldLimit = 500;
    officer.bankTabRights[1] = 7;
    g.rosterReceived(rosterWith({}, {master, officer}));

    g.selectRank(2);
    CHECK(g.rankDraft().rights == 0x3);
    CHECK(g.rankDraft().goldLimit == 500);
    CHECK(g.rankDraft().tabRights[1] == 7);

    g.selectRank(9);
    CHECK(g.rankDraft().rights == 0);
}

TEST_CASE("The player's rank and rights come from the roster", "[guild]") {
    Guild g;
    CHECK(g.rankIndexOf(42) == Guild::kNoRank);
    Rank master;
    master.rights = 0xFFFF;
    Rank member_;
    member_.rights = 0x41;
    g.rosterReceived(rosterWith({member("Me", true, 42, 1)}, {master, member_}));
    CHECK(g.rankIndexOf(42) == 1);
    CHECK(g.rightsOf(42) == 0x41);
    CHECK(g.rightsOf(99) == 0);
}

TEST_CASE("The next character has none of the last one's guild", "[guild]") {
    Guild g;
    g.rosterReceived(rosterWith({}));
    Identity own;
    own.guildId = 7;
    own.guildName = "Horde Inc";
    g.identityReceived(own, 7);
    g.inviteReceived("Thrall", "Horde Inc");
    g.eventLogReceived({EventLogEntry{}});
    g.beginCharacter();
    CHECK_FALSE(g.isMember(0));
    CHECK(g.name().empty());
    CHECK_FALSE(g.hasPendingInvite());
    CHECK(g.eventLog().empty());
}

TEST_CASE("The guild summary is news only when it changes", "[guild]") {
    Guild g;
    Summary s;
    s.guildName = "Horde Inc";
    s.numMembers = 40;
    CHECK(has<SummaryChanged>(g.summaryReceived(s, 7)));
    CHECK_FALSE(has<SummaryChanged>(g.summaryReceived(s, 7)));
    s.numMembers = 41;
    CHECK(first<SummaryChanged>(g.summaryReceived(s, 7)).members == 41);
}

TEST_CASE("An unknown guild name is asked for once, and again only after two seconds", "[guild]") {
    GuildNames names;
    const auto t0 = GuildNames::Clock::time_point{} + std::chrono::hours(1);
    CHECK(names.shouldQuery(5, t0));
    CHECK_FALSE(names.shouldQuery(5, t0 + std::chrono::seconds(1)));
    CHECK(names.shouldQuery(5, t0 + std::chrono::seconds(3)));
    CHECK(names.learn(5, "Horde Inc"));
    CHECK_FALSE(names.learn(5, "Horde Inc"));
    CHECK_FALSE(names.shouldQuery(5, t0 + std::chrono::seconds(10)));
    CHECK(names.find(5) == "Horde Inc");
}
