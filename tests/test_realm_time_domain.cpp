// The realm's time end to end: the packets that say what time it is on the
// realm in, the realm clock and the world's hour read back out through
// RealmTimeHandler, and the clock running on between them.
//
// The realm clock is what makes something drawn from time - the wind - come
// out the same on every client of a realm, so what matters is that it reads
// what the server said and moves on at the rate real time does.

#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "game/packed_time.hpp"
#include "game/realm_time_handler.hpp"

#include <chrono>
#include <ctime>

using Catch::Approx;
using wowee::game::Opcode;
using wowee::game::RealmClock;
using wowee::game::WowDate;
using wowee::test::Body;
using wowee::test::GameHarness;

namespace {

/// 2025-10-09 08:53:20 UTC.
constexpr uint32_t kUnixTime = 1760000000u;

/// SMSG_QUERY_TIME_RESPONSE as WotLK servers send it: the time, and the
/// seconds until the daily quests reset.
std::vector<uint8_t> queryTimeResponse(uint32_t unixTime) {
    return Body().u32(unixTime).u32(3600);
}

/// SMSG_LOGIN_SETTIMESPEED: a packed date and the speed, 1/60 being real time.
std::vector<uint8_t> loginSetTimeSpeed(const WowDate& date) {
    return Body().u32(wowee::game::packWowPackedTime(date)).f32(1.0f / 60.0f);
}

WowDate date(int year, int month, int day, int hour, int minute) {
    WowDate d;
    d.yearSince2000 = year - 2000;
    d.month = month;
    d.day = day;
    d.hour = hour;
    d.minute = minute;
    return d;
}

/// What the realm clock reads now.
double realmSeconds(GameHarness& h) {
    const auto seconds = h.game().realmTime().realmSeconds();
    REQUIRE(seconds.has_value());
    return *seconds;
}

}  // namespace

TEST_CASE("The realm clock is not known before the server says what time it is",
          "[realm_time]") {
    GameHarness h;
    h.enterWorld();
    CHECK_FALSE(h.game().realmTime().realmSeconds().has_value());
    CHECK(h.game().realmTime().realmClockSource() == RealmClock::Source::None);
}

TEST_CASE("The server's Unix time sets the realm clock to the second, and it runs on",
          "[realm_time]") {
    GameHarness h;
    h.enterWorld();

    h.receive(Opcode::SMSG_QUERY_TIME_RESPONSE, queryTimeResponse(kUnixTime));
    h.pump();
    // Read straight after: at most the frame the reply was handled in has
    // passed since.
    CHECK(realmSeconds(h) == Approx(kUnixTime).margin(0.05));
    CHECK(h.game().realmTime().realmClockSource() == RealmClock::Source::UnixTime);

    const double before = realmSeconds(h);
    h.advance(10.0f);
    CHECK(realmSeconds(h) - before == Approx(10.0).margin(0.05));
}

TEST_CASE("The login packet's date sets the realm clock to the minute until the Unix time comes",
          "[realm_time]") {
    GameHarness h;
    h.enterWorld();

    // 2025-10-09 08:53, realm-local - read as though it were UTC.
    h.receive(Opcode::SMSG_LOGIN_SETTIMESPEED, loginSetTimeSpeed(date(2025, 10, 9, 8, 53)));
    h.pump();
    CHECK(realmSeconds(h) == Approx(1759999980.0).margin(0.05));
    CHECK(h.game().realmTime().realmClockSource() == RealmClock::Source::PackedDate);
    // The same packet still sets the world's hour.
    CHECK(h.game().getGameTime() == Approx(8.0f + 53.0f / 60.0f).margin(0.01f));

    const double before = realmSeconds(h);
    h.advance(5.0f);
    CHECK(realmSeconds(h) - before == Approx(5.0).margin(0.05));

    // The Unix time, a moment later, replaces it: it is the finer source.
    h.receive(Opcode::SMSG_QUERY_TIME_RESPONSE, queryTimeResponse(kUnixTime + 7200));
    h.pump();
    CHECK(realmSeconds(h) == Approx(kUnixTime + 7200.0).margin(0.05));

    // And a packed date after that says nothing the clock does not know
    // better: the game-time packets move the hour and leave the clock alone.
    h.receive(Opcode::SMSG_GAMETIME_SET,
              Body().u32(wowee::game::packWowPackedTime(date(2025, 10, 9, 20, 0))));
    h.pump();
    CHECK(realmSeconds(h) == Approx(kUnixTime + 7200.0).margin(0.05));
    CHECK(h.game().getGameTime() == Approx(20.0f).margin(0.01f));
}

TEST_CASE("A login packet carrying no date leaves the realm clock unknown", "[realm_time]") {
    GameHarness h;
    h.enterWorld();

    // A zero word unpacks to 1 January 2000 at midnight: no date at all.
    h.receive(Opcode::SMSG_LOGIN_SETTIMESPEED, Body().u32(0).f32(1.0f / 60.0f));
    h.pump();
    CHECK_FALSE(h.game().realmTime().realmSeconds().has_value());
}

TEST_CASE("Two clients of one realm read the same realm clock", "[realm_time]") {
    // Logged in at different moments, each told the time when it asked.
    GameHarness first;
    first.enterWorld();
    first.receive(Opcode::SMSG_QUERY_TIME_RESPONSE, queryTimeResponse(kUnixTime));
    first.pump();

    GameHarness second;
    second.enterWorld();
    second.receive(Opcode::SMSG_QUERY_TIME_RESPONSE, queryTimeResponse(kUnixTime + 30));
    second.pump();

    // Thirty seconds on for the first, which is where the second came in.
    first.advance(30.0f);
    CHECK(realmSeconds(first) == Approx(realmSeconds(second)).margin(0.1));
}
