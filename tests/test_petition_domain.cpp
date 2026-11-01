// Guild and arena charters, end to end: the registrar's offer and the charter's
// signatures as the server sends them, what the interface is told and reads,
// the server's answers, and the requests that go back.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_handler.hpp"
#include "petition/services/petitions.hpp"

#include <algorithm>
#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;

namespace {

constexpr uint64_t kRegistrar = 0xF130000000000050ull;
constexpr uint64_t kCharter = 0x4000000000000077ull;
constexpr uint64_t kOwner = 0x20;
constexpr uint64_t kSigner = 0x21;
constexpr uint32_t kGuildCharterItem = 5863;

struct Offered {
    uint32_t itemId;
    uint32_t displayId;
    uint32_t cost;
};

/// SMSG_PETITION_SHOWLIST: the registrar, a one-byte count, then six words per
/// charter starting with its index.
std::vector<uint8_t> showlist(const std::vector<Offered>& charters) {
    Body b;
    b.u64(kRegistrar).u8(static_cast<uint8_t>(charters.size()));
    uint32_t index = 1;
    for (const auto& c : charters) b.u32(index++).u32(c.itemId).u32(c.displayId).u32(c.cost).u32(0).u32(9);
    return b.bytes();
}

/// SMSG_PETITION_SHOW_SIGNATURES: the charter, its owner, its item, then each
/// signer and a word the client skips.
std::vector<uint8_t> signatures(const std::vector<uint64_t>& signers) {
    Body b;
    b.u64(kCharter).u64(kOwner).u32(kGuildCharterItem).u8(static_cast<uint8_t>(signers.size()));
    for (uint64_t g : signers) b.u64(g).u32(0);
    return b.bytes();
}

/// SMSG_PETITION_QUERY_RESPONSE, as far as the client reads it: the name, the
/// signatures needed (twice), and the arena team size or zero for a guild.
std::vector<uint8_t> details(const std::string& name, uint32_t needed, uint32_t arenaType) {
    return Body().u32(kGuildCharterItem).u64(kCharter).cstr(name).cstr("").u32(needed).u32(needed).u32(arenaType).bytes();
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

TEST_CASE("a guild registrar's offer opens the guild panel and is priced from it", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_PETITION_SHOWLIST, showlist({{kGuildCharterItem, 16161, 1000}}));
    h.pump();

    CHECK(h.addonEvents("GUILD_REGISTRAR_SHOW").size() == 1);
    CHECK(h.addonEvents("PETITION_VENDOR_SHOW").empty());
    CHECK(h.petitions().getPetitionCost() == 1000);
    CHECK(h.petitions().getPetitionNpcGuid() == kRegistrar);

    // Closing names the panel that opened, and only once.
    h.petitions().closePetitionVendor();
    h.petitions().closePetitionVendor();
    h.pump();
    CHECK(h.addonEvents("GUILD_REGISTRAR_CLOSED").size() == 1);
    CHECK(h.addonEvents("PETITION_VENDOR_CLOSED").empty());
}

TEST_CASE("an arena registrar opens its own panel and offers all three charters", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_PETITION_SHOWLIST,
              showlist({{23560, 1, 800000}, {23561, 2, 1200000}, {23562, 3, 2000000}}));
    h.pump();

    CHECK(h.addonEvents("PETITION_VENDOR_SHOW").size() == 1);
    uint32_t itemId = 0, displayId = 0, cost = 0;
    REQUIRE(h.petitions().getPetitionCharter(2, itemId, displayId, cost));
    CHECK(itemId == 23562);
    CHECK(cost == 2000000);
    CHECK_FALSE(h.petitions().getPetitionCharter(3, itemId, displayId, cost));

    // The tab that was open is the only thing telling a five-person charter
    // from a two-person one.
    h.petitions().buyPetition(h.petitions().getPetitionNpcGuid(), "Gladiators", 3);
    auto* buy = h.lastSent(Opcode::CMSG_PETITION_BUY);
    REQUIRE(buy);
    CHECK(buy->readUInt64() == kRegistrar);
    buy->readUInt32();
    buy->readUInt64();
    CHECK(buy->readString() == "Gladiators");
}

TEST_CASE("a charter's signatures open its frame and ask what the charter is", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_PETITION_SHOW_SIGNATURES, signatures({kSigner}));
    h.pump();

    CHECK(h.addonEvents("PETITION_SHOW").size() == 1);
    const auto& info = h.petitions().getPetitionInfo();
    CHECK(info.petitionGuid == kCharter);
    CHECK(info.ownerGuid == kOwner);
    REQUIRE(info.signatures.size() == 1);
    CHECK(info.signatures[0].playerGuid == kSigner);
    auto* query = h.lastSent(Opcode::CMSG_PETITION_QUERY);
    REQUIRE(query);
    CHECK(query->readUInt32() == kGuildCharterItem);
    CHECK(query->readUInt64() == kCharter);

    h.receive(Opcode::SMSG_PETITION_QUERY_RESPONSE, details("Gladiators", 4, 2));
    h.pump();

    // The frame drew when the signatures came; told again, it redraws named.
    CHECK(h.addonEvents("PETITION_SHOW").size() == 2);
    CHECK(h.petitions().getPetitionInfo().guildName == "Gladiators");
    CHECK(h.petitions().getPetitionInfo().signaturesRequired == 4);
    CHECK(h.petitions().getPetitionInfo().isArena);
}

TEST_CASE("a signature the server confirms goes onto the charter shown", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PETITION_SHOW_SIGNATURES, signatures({}));
    h.pump();

    h.receive(Opcode::SMSG_PETITION_SIGN_RESULTS, Body().u64(kCharter).u64(kSigner).u32(0));
    h.pump();

    CHECK(chatSaid(h, "Petition signed successfully."));
    CHECK(h.petitions().getPetitionInfo().signatureCount == 1);
    REQUIRE(h.petitions().getPetitionInfo().signatures.size() == 1);
    CHECK(h.petitions().getPetitionInfo().signatures[0].playerGuid == kSigner);
}

TEST_CASE("a refused signature says why and adds nobody", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_PETITION_SHOW_SIGNATURES, signatures({}));
    h.pump();

    h.receive(Opcode::SMSG_PETITION_SIGN_RESULTS, Body().u64(kCharter).u64(kSigner).u32(3));
    h.receive(Opcode::SMSG_PETITION_SIGN_RESULTS, Body().u64(kCharter).u64(kSigner).u32(7));
    h.pump();

    CHECK(uiErrors(h) == std::vector<std::string>{"You cannot sign your own petition."});
    CHECK(chatSaid(h, "Cannot sign petition (error 7)."));
    CHECK(h.petitions().getPetitionInfo().signatures.empty());
}

TEST_CASE("turning a charter in reports the guild made, or why not", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_TURN_IN_PETITION_RESULTS, Body().u32(2));
    h.receive(Opcode::SMSG_TURN_IN_PETITION_RESULTS, Body().u32(0));
    h.pump();

    CHECK(chatSaid(h, "Guild creation failed: not enough signatures."));
    CHECK(chatSaid(h, "Guild created successfully!"));
}

TEST_CASE("a charter that could not be offered says why", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_OFFER_PETITION_ERROR, Body().u32(1));
    h.receive(Opcode::SMSG_OFFER_PETITION_ERROR, Body().u32(5));
    h.pump();

    CHECK(chatSaid(h, "Player is already in a guild."));
    CHECK(uiErrors(h) == std::vector<std::string>{"Cannot offer petition to that player."});
}

TEST_CASE("signing, offering, turning in and renaming name the charter", "[petition][harness]") {
    GameHarness h;
    h.enterWorld();

    h.petitions().signPetition(kCharter);
    h.petitions().offerPetition(kCharter, kSigner);
    h.petitions().turnInPetition(kCharter);
    h.petitions().renamePetition(kCharter, "Renamed");

    auto* sign = h.lastSent(Opcode::CMSG_PETITION_SIGN);
    REQUIRE(sign);
    CHECK(sign->readUInt64() == kCharter);
    auto* offer = h.lastSent(Opcode::CMSG_OFFER_PETITION);
    REQUIRE(offer);
    offer->readUInt32();  // read and thrown away by the server
    CHECK(offer->readUInt64() == kCharter);
    CHECK(offer->readUInt64() == kSigner);
    auto* turnIn = h.lastSent(Opcode::CMSG_TURN_IN_PETITION);
    REQUIRE(turnIn);
    CHECK(turnIn->readUInt64() == kCharter);
    auto* rename = h.lastSent(Opcode::MSG_PETITION_RENAME);
    REQUIRE(rename);
    CHECK(rename->readUInt64() == kCharter);
    CHECK(rename->readString() == "Renamed");
}

TEST_CASE("the next character is not at the last one's registrar or charter", "[petition][harness]") {
    PlayerSpec alice;
    alice.guid = 0x71;
    alice.name = "Alice";
    PlayerSpec bob;
    bob.guid = 0x72;
    bob.name = "Bob";

    GameHarness h;
    h.enterWorld(alice);
    h.receive(Opcode::SMSG_PETITION_SHOWLIST, showlist({{kGuildCharterItem, 16161, 1000}}));
    h.receive(Opcode::SMSG_PETITION_SHOW_SIGNATURES, signatures({kSigner}));
    h.pump();
    REQUIRE(h.petitions().getPetitionInfo().petitionGuid == kCharter);

    h.relogAs(bob);

    CHECK(h.petitions().getPetitionInfo().petitionGuid == 0);
    CHECK(h.petitions().getPetitionInfo().signatures.empty());
    CHECK(h.petitions().getPetitionNpcGuid() == 0);
    // No panel of Bob's is open, so closing tells the interface nothing.
    h.petitions().closePetitionVendor();
    h.pump();
    CHECK(h.addonEvents("GUILD_REGISTRAR_CLOSED").empty());
}
