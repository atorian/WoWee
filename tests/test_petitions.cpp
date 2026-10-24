// Charters: the registrar's offer and the signatures on a charter, and the
// service against a recording gateway and the real event bus. Only
// wowee_petition is linked.
#include <catch_amalgamated.hpp>

#include "core/event_bus.hpp"
#include "petition/services/petition_service.hpp"

#include <string>
#include <variant>
#include <vector>

using namespace wowee::petition;

namespace {

CharterOffer offer(bool guild) {
    CharterOffer o;
    o.npcGuid = 50;
    o.guildCharter = guild;
    o.cost = 1000;
    o.charters = {{5863, 11, 1000}};
    if (!guild) o.charters = {{23560, 21, 800}, {23561, 22, 1200}, {23562, 23, 2000}};
    return o;
}

std::vector<PetitionSignature> signatures(std::initializer_list<uint64_t> guids) {
    std::vector<PetitionSignature> out;
    for (uint64_t g : guids) {
        PetitionSignature s;
        s.playerGuid = g;
        out.push_back(s);
    }
    return out;
}

struct RecordingGateway final : IPetitionGateway {
    bool world = true, socket = true;
    std::vector<std::string> sent;

    bool online() const override { return world && socket; }
    bool connected() const override { return socket; }
    void sendBuy(uint64_t npc, const std::string& name, uint32_t index) override {
        sent.push_back("buy " + std::to_string(npc) + " " + name + " " + std::to_string(index));
    }
    void sendSign(uint64_t g) override { sent.push_back("sign " + std::to_string(g)); }
    void sendTurnIn(uint64_t g) override { sent.push_back("turn in " + std::to_string(g)); }
    void sendOffer(uint64_t g, uint64_t t) override { sent.push_back("offer " + std::to_string(g) + " to " + std::to_string(t)); }
    void sendRename(uint64_t g, const std::string& name) override { sent.push_back("rename " + std::to_string(g) + " " + name); }
    void sendQuery(uint32_t entry, uint64_t g) override { sent.push_back("query " + std::to_string(entry) + " " + std::to_string(g)); }
};

struct Fixture {
    RecordingGateway gateway;
    wowee::core::EventBus bus;
    PetitionService service{gateway, bus};
    std::vector<std::string> events;
    std::vector<wowee::core::EventBus::Subscription> subscriptions;

    Fixture() {
        subscriptions.push_back(bus.subscribe<VendorShown>(
            [this](const VendorShown& e) { events.push_back(e.guild ? "guild shown" : "arena shown"); }));
        subscriptions.push_back(bus.subscribe<VendorClosed>(
            [this](const VendorClosed& e) { events.push_back(e.guild ? "guild closed" : "arena closed"); }));
        subscriptions.push_back(bus.subscribe<PetitionShown>([this](const auto&) { events.push_back("petition"); }));
        subscriptions.push_back(bus.subscribe<PetitionSigned>([this](const auto&) { events.push_back("signed"); }));
        subscriptions.push_back(bus.subscribe<SignRefused>(
            [this](const SignRefused& e) { events.push_back("refused " + std::to_string(e.code)); }));
    }
};

} // namespace

TEST_CASE("Closing names the panel that opened, once", "[petition]") {
    Petitions p;
    CHECK(p.vendorClosed().empty());
    p.vendorShown(offer(false));
    auto closed = p.vendorClosed();
    REQUIRE(closed.size() == 1);
    CHECK_FALSE(std::get<VendorClosed>(closed[0]).guild);
    CHECK(p.vendor() == 0);
    CHECK(p.vendorClosed().empty());
}

TEST_CASE("Each charter on offer can be asked for, and no more", "[petition]") {
    Petitions p;
    p.vendorShown(offer(false));
    REQUIRE(p.charter(2));
    CHECK(p.charter(2)->itemId == 23562);
    CHECK_FALSE(p.charter(3));
    CHECK_FALSE(p.charter(-1));
}

TEST_CASE("A new charter forgets the last one's details", "[petition]") {
    Petitions p;
    p.signaturesShown(1, 9, 1, signatures({30}));
    p.detailsReceived(1, "Old", 4, true);
    p.signaturesShown(2, 9, 0, {});
    CHECK(p.info().petitionGuid == 2);
    CHECK(p.info().guildName.empty());
    CHECK(p.info().signaturesRequired == 9);
    CHECK_FALSE(p.info().isArena);
}

TEST_CASE("A new character is not at the last one's registrar or charter", "[petition]") {
    Petitions p;
    p.vendorShown(offer(true));
    p.signaturesShown(1, 9, 1, signatures({30}));
    p.beginCharacter();
    CHECK(p.vendor() == 0);
    CHECK_FALSE(p.charter(0));
    CHECK(p.info().petitionGuid == 0);
    CHECK(p.info().signatures.empty());
    // No panel is open to close.
    CHECK(p.vendorClosed().empty());
}

TEST_CASE("Details apply only to the charter shown", "[petition]") {
    Petitions p;
    p.signaturesShown(1, 9, 0, {});
    CHECK(p.detailsReceived(2, "Other", 4, false).empty());
    CHECK(p.info().guildName.empty());
    CHECK_FALSE(p.detailsReceived(1, "Mine", 0, true).empty());
    CHECK(p.info().guildName == "Mine");
    CHECK(p.info().signaturesRequired == 9);   // zero says nothing
    CHECK(p.info().isArena);
}

TEST_CASE("The stated count is kept though the list is short", "[petition]") {
    Petitions p;
    p.signaturesShown(1, 9, 4, signatures({30, 31}));
    CHECK(p.info().signatureCount == 4);
    CHECK(p.info().signatures.size() == 2);
}

TEST_CASE("Showing signatures asks what the charter is", "[petition]") {
    Fixture f;
    f.service.onSignatures(1, 9, 5863, 0, {});
    CHECK(f.gateway.sent == std::vector<std::string>{"query 5863 1"});
    CHECK(f.events == std::vector<std::string>{"petition"});
}

TEST_CASE("A signature counts on success only, and on the charter shown", "[petition]") {
    Fixture f;
    f.service.onSignatures(1, 9, 5863, 1, signatures({30}));
    f.service.onSigned(1, 31);
    f.service.onSignRefused(SignRefusal::AlreadySigned, 1);
    f.service.onSigned(2, 33);
    CHECK(f.service.getPetitionInfo().signatureCount == 2);
    CHECK(f.service.getPetitionInfo().signatures.back().playerGuid == 31);
    CHECK(f.events == std::vector<std::string>{"petition", "signed", "refused 1", "signed"});
}

TEST_CASE("The registrar's offer reads back", "[petition]") {
    Fixture f;
    f.service.onVendor(offer(true));
    uint32_t itemId = 0, displayId = 0, cost = 0;
    REQUIRE(f.service.getPetitionCharter(0, itemId, displayId, cost));
    CHECK(itemId == 5863);
    CHECK(displayId == 11);
    CHECK(cost == 1000);
    CHECK_FALSE(f.service.getPetitionCharter(1, itemId, displayId, cost));
    CHECK(f.service.getPetitionNpcGuid() == 50);
    CHECK(f.events == std::vector<std::string>{"guild shown"});
}

TEST_CASE("Offering and renaming need a charter, and someone or a name", "[petition]") {
    Fixture f;
    f.service.offerPetition(0, 5);
    f.service.offerPetition(1, 0);
    f.service.renamePetition(0, "Name");
    f.service.renamePetition(1, "");
    CHECK(f.gateway.sent.empty());
    f.service.offerPetition(1, 5);
    f.service.renamePetition(1, "Name");
    CHECK(f.gateway.sent == std::vector<std::string>{"offer 1 to 5", "rename 1 Name"});
}

TEST_CASE("Nothing is sent outside the world; the query needs only a connection", "[petition]") {
    Fixture f;
    f.gateway.world = false;
    f.service.buyPetition(50, "Guild");
    f.service.signPetition(1);
    f.service.turnInPetition(1);
    f.service.onSignatures(1, 9, 5863, 0, {});
    CHECK(f.gateway.sent == std::vector<std::string>{"query 5863 1"});
}
