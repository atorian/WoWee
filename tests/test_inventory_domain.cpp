// Looting money, end to end: the loot window's packets, what the player is
// told, and what is counted.
#include <catch_amalgamated.hpp>

#include "game_harness.hpp"

#include "game/game_events.hpp"

#include <string>
#include <vector>

using wowee::game::Opcode;
using wowee::test::Body;
using wowee::test::GameHarness;
namespace ev = wowee::game::events;

namespace {

constexpr uint64_t kCorpse = 0xF130000001000001ull;

/// SMSG_LOOT_RESPONSE for a corpse holding money and nothing else, as 3.3.5a
/// writes it: the loot guid, the loot type, the money, no items and no quest
/// items.
std::vector<uint8_t> moneyLoot(uint64_t lootGuid, uint32_t copper) {
    return Body().u64(lootGuid).u8(1 /* corpse */).u32(copper).u8(0).u8(0);
}

/// Every LootMoneyReceived the game announces, while it lives.
struct MoneyAnnounced {
    explicit MoneyAnnounced(GameHarness& h)
        : sub(h.events().subscribe<ev::LootMoneyReceived>(
              [this](const ev::LootMoneyReceived& e) { seen.push_back(e); })) {}
    std::vector<ev::LootMoneyReceived> seen;
    wowee::core::EventBus::Subscription sub;
};

size_t lootedLines(GameHarness& h) {
    size_t n = 0;
    for (const auto& line : h.game().getChatHistory()) {
        if (line.message.rfind("Looted: ", 0) == 0) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("money the server confirms is taken, counted and announced once", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    MoneyAnnounced money(h);
    const uint64_t before = h.game().getMoneyCopper();

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 1234));
    h.pump();
    CHECK(h.addonEvents("LOOT_OPENED").size() == 1);
    CHECK(h.countSent(Opcode::CMSG_LOOT_MONEY) == 1);

    h.receive(Opcode::SMSG_LOOT_MONEY_NOTIFY, Body().u32(1234).u8(1));
    h.pump();
    REQUIRE(money.seen.size() == 1);
    CHECK(money.seen[0].copper == 1234);
    CHECK(money.seen[0].lootGuid == kCorpse);
    CHECK(h.game().getMoneyCopper() == before + 1234);
    CHECK(h.addonEvents("PLAYER_MONEY").size() == 1);

    // The fallback for a server that never confirms must not say it again.
    h.advance(1.0f);
    CHECK(money.seen.size() == 1);
    CHECK(lootedLines(h) == 1);
}

TEST_CASE("money a server never confirms is still announced, once", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    MoneyAnnounced money(h);

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 500));
    h.pump();
    CHECK(money.seen.empty());

    h.advance(1.0f);
    REQUIRE(money.seen.size() == 1);
    CHECK(money.seen[0].copper == 500);
    CHECK(lootedLines(h) == 1);
}

TEST_CASE("the same container looted again later is announced again", "[loot][harness]") {
    GameHarness h;
    h.enterWorld();
    MoneyAnnounced money(h);

    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 100));
    h.pump();
    h.advance(1.0f);
    h.receive(Opcode::SMSG_LOOT_RELEASE_RESPONSE, Body().u64(kCorpse).u8(1));
    h.pump();
    REQUIRE(money.seen.size() == 1);

    // The cooldown that stops one loot being announced twice runs out. It
    // was set in InventoryHandler and counted down in a GameHandler copy
    // nothing set, so it never ran out and this container stayed silent for
    // the rest of the session.
    h.advance(2.0f);
    h.receive(Opcode::SMSG_LOOT_RESPONSE, moneyLoot(kCorpse, 100));
    h.pump();
    h.advance(1.0f);
    CHECK(money.seen.size() == 2);
}
