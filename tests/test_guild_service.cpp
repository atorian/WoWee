// The guild service against a recording gateway and the real event bus.
#include <catch_amalgamated.hpp>

#include "foundation/event_bus.hpp"
#include "guild/services/guild_service.hpp"
#include "guild_event_fixture.hpp"

#include <string>
#include <vector>

using namespace wowee::guild;

namespace {

struct RecordingGateway final : IGuildGateway, IGuildMember {
    bool inWorld = true;
    uint32_t guildId = 0;
    std::vector<std::string> sent;

    bool canSend() const override { return inWorld; }
    bool canQuery() const override { return true; }
    void requestInfo() override { sent.push_back("info"); }
    void requestRoster() override { sent.push_back("roster"); }
    void requestEventLog() override { sent.push_back("log"); }
    void queryGuild(uint32_t id) override { sent.push_back("query " + std::to_string(id)); }
    void sendInfoText(const std::string&) override { sent.push_back("infotext"); }
    void sendMotd(const std::string&) override { sent.push_back("motd"); }
    void sendPromote(const std::string& n) override { sent.push_back("promote " + n); }
    void sendDemote(const std::string& n) override { sent.push_back("demote " + n); }
    void sendLeave() override { sent.push_back("leave"); }
    void sendInvite(const std::string& n) override { sent.push_back("invite " + n); }
    void sendRemove(const std::string& n) override { sent.push_back("remove " + n); }
    void sendDisband() override { sent.push_back("disband"); }
    void sendLeader(const std::string& n) override { sent.push_back("leader " + n); }
    void sendPublicNote(const std::string&, const std::string&) override { sent.push_back("pnote"); }
    void sendOfficerNote(const std::string&, const std::string&) override { sent.push_back("onote"); }
    void sendAcceptInvite() override { sent.push_back("accept"); }
    void sendDeclineInvite() override { sent.push_back("decline"); }
    void sendCreate(const std::string&) override { sent.push_back("create"); }
    void sendAddRank(const std::string&) override { sent.push_back("addrank"); }
    void sendDeleteRank() override { sent.push_back("delrank"); }
    void sendRank(uint32_t id, const std::string& name, const RankDraft& r) override {
        sent.push_back("rank " + std::to_string(id) + " " + name + " gold " + std::to_string(r.goldLimit));
    }
    uint64_t playerGuid() const override { return 42; }
    uint32_t ownGuildId() const override { return guildId; }
};

struct MemoryStore final : IGuildNameStore {
    std::unordered_map<uint32_t, std::string> saved;
    int saves = 0;
    std::unordered_map<uint32_t, std::string> load() const override { return saved; }
    void save(const std::unordered_map<uint32_t, std::string>& names) override { saved = names; ++saves; }
};

struct Fixture {
    RecordingGateway gateway;
    MemoryStore store;
    wowee::core::EventBus bus;
    std::vector<std::string> notices;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<PlayerNameRequired>([this](const auto&) { notices.push_back("name required"); }));
        subscriptions.push_back(bus.subscribe<InviteRequested>([this](const InviteRequested& e) { notices.push_back("inviting " + e.name); }));
    }
};

} // namespace

TEST_CASE("Saving a rank sends it zero-based with the panel's draft", "[guild]") {
    Fixture f;
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    Roster roster;
    roster.ranks.resize(3);
    service.onRoster(roster);
    f.gateway.sent.clear();

    service.setSelectedGuildRank(2);
    service.pendingGuildRankRef().goldLimit = 500;
    service.saveGuildRank("Officer");
    CHECK(f.gateway.sent == std::vector<std::string>{"rank 1 Officer gold 500", "roster"});
}

TEST_CASE("A guild name is saved when learned, and not again when repeated", "[guild]") {
    Fixture f;
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    Identity other;
    other.guildId = 9;
    other.guildName = "Alliance Ltd";
    service.onIdentity(other);
    service.onIdentity(other);
    CHECK(f.store.saves == 1);
    CHECK(f.store.saved.at(9) == "Alliance Ltd");
}

TEST_CASE("Names remembered from an earlier session are not asked for", "[guild]") {
    Fixture f;
    f.store.saved[9] = "Alliance Ltd";
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    CHECK(service.lookupGuildName(9) == "Alliance Ltd");
    CHECK(service.lookupGuildName(10).empty());
    CHECK(f.gateway.sent == std::vector<std::string>{"query 10"});
}

TEST_CASE("An outdated roster is asked for again, a tabard change re-queries the guild", "[guild]") {
    Fixture f;
    f.gateway.guildId = 7;
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    Identity own;
    own.guildId = 7;
    own.guildName = "Horde Inc";
    service.onIdentity(own);
    service.onRoster(Roster{});
    f.gateway.sent.clear();

    service.onEvent(guildEvent(GuildEvent::JOINED, {"Rexxar"}));
    CHECK(f.gateway.sent == std::vector<std::string>{"roster"});

    f.gateway.sent.clear();
    service.onEvent(guildEvent(GuildEvent::TABARD_CHANGED));
    CHECK(f.gateway.sent == std::vector<std::string>{"query 7"});
}

TEST_CASE("A roster for a guild with no name yet asks for the name", "[guild]") {
    Fixture f;
    f.gateway.guildId = 7;
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    service.onRoster(Roster{});
    CHECK(f.gateway.sent == std::vector<std::string>{"query 7"});
}

TEST_CASE("An invite without a name is refused, outside the world nothing is sent", "[guild]") {
    Fixture f;
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    service.inviteToGuild("");
    service.inviteToGuild("Rexxar");
    CHECK(f.gateway.sent == std::vector<std::string>{"invite Rexxar"});
    CHECK(f.notices == std::vector<std::string>{"name required", "inviting Rexxar"});

    f.gateway.inWorld = false;
    f.gateway.sent.clear();
    service.inviteToGuild("Jaina");
    service.leaveGuild();
    CHECK(f.gateway.sent.empty());
}

TEST_CASE("Accepting an invite asks for the roster that fills the guild tab", "[guild]") {
    Fixture f;
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    service.onInvite("Thrall", "Horde Inc");
    service.acceptGuildInvite();
    CHECK(f.gateway.sent == std::vector<std::string>{"accept", "roster"});
    CHECK_FALSE(service.hasPendingGuildInvite());
}

TEST_CASE("Leaving answers no to isInGuild once the player's field is cleared", "[guild]") {
    Fixture f;
    f.gateway.guildId = 7;
    GuildService service(f.gateway, f.gateway, f.store, f.bus);
    service.onRoster(Roster{});
    CHECK(service.isInGuild());
    service.onCommandResult(2, 0, "");
    f.gateway.guildId = 0;
    CHECK_FALSE(service.isInGuild());
}
