// The realm clock between the packets that set it.
//
// The server sends the hour once at login and leaves the client to run it;
// keeping only that hour froze the sky at the login time for the session.
#include <catch_amalgamated.hpp>
#include "game/game_clock.hpp"

using wowee::game::GameClock;
using namespace std::chrono_literals;

TEST_CASE("game clock is unknown until the server sets it") {
    GameClock c;
    CHECK_FALSE(c.known());
    CHECK(c.hours() == -1.0f);
}

TEST_CASE("game clock runs at real time on the speed servers send") {
    GameClock c;
    const auto t0 = GameClock::Clock::time_point{} + 1000s;
    c.set(14.5f, 1.0f / 60.0f, t0);
    CHECK(c.hours(t0) == Catch::Approx(14.5f));
    CHECK(c.hours(t0 + 30min) == Catch::Approx(15.0f));
    CHECK(c.hours(t0 + 3h) == Catch::Approx(17.5f));
}

TEST_CASE("game clock wraps past midnight") {
    GameClock c;
    const auto t0 = GameClock::Clock::time_point{} + 1000s;
    c.set(23.5f, GameClock::kRealTimeSpeed, t0);
    CHECK(c.hours(t0 + 1h) == Catch::Approx(0.5f));
    CHECK(c.hours(t0 + 24h) == Catch::Approx(23.5f));
}

TEST_CASE("game clock follows a faster speed") {
    GameClock c;
    const auto t0 = GameClock::Clock::time_point{} + 1000s;
    c.set(6.0f, 1.0f, t0);   // a game minute every real second
    CHECK(c.hours(t0 + 60s) == Catch::Approx(7.0f));
}

TEST_CASE("game clock treats an unusable speed as real time") {
    GameClock c;
    const auto t0 = GameClock::Clock::time_point{} + 1000s;
    c.set(12.0f, 0.0f, t0);
    CHECK(c.speed() == GameClock::kRealTimeSpeed);
    CHECK(c.hours(t0 + 1h) == Catch::Approx(13.0f));
}

TEST_CASE("an hour without a speed keeps the speed set before") {
    GameClock c;
    const auto t0 = GameClock::Clock::time_point{} + 1000s;
    c.set(6.0f, 1.0f, t0);
    c.setHours(20.0f, t0 + 10s);
    CHECK(c.hours(t0 + 70s) == Catch::Approx(21.0f));
}
