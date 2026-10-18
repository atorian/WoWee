// The weather end to end: SMSG_WEATHER in as a WotLK realm writes it, the
// weather the game reads out of it through WeatherHandler, what it says on the
// bus for the client to draw and sound, and what the interface is told.
//
// The realm's number carries the kind and its grade together (WeatherState:
// 3/4/5 rain, 6/7/8 snow, 22/41/42 sandstorm, 86 thunder, 90/106 black rain
// and snow). The client once read it as 1 rain, 2 snow, 3 storm - so a light
// shower was a storm, and heavy rain, every snow and every sandstorm were
// nothing at all.

#include <catch_amalgamated.hpp>
#include "game_harness.hpp"
#include "game/game_events.hpp"
#include "game/weather_handler.hpp"

#include <vector>

using Catch::Approx;
using wowee::game::Opcode;
using wowee::game::WeatherNumbering;
using wowee::game::decodeWeather;
using wowee::pipeline::WeatherKind;
using wowee::test::Body;
using wowee::test::GameHarness;
using wowee::test::PlayerSpec;
namespace events = wowee::game::events;

namespace {

/// SMSG_WEATHER as AzerothCore's Weather::SendWeatherUpdateToPlayer writes it:
/// the state, the grade and whether it comes at once.
std::vector<uint8_t> weather(uint32_t state, float grade, bool abrupt = false) {
    return Body().u32(state).f32(grade).u8(abrupt ? 1 : 0);
}

/// What the game says on the bus about the weather, in order.
struct Heard {
    std::vector<events::WeatherChanged> changes;
    wowee::core::EventBus::Subscription subscription;
    explicit Heard(GameHarness& h)
        : subscription(h.events().subscribe<events::WeatherChanged>(
              [this](const events::WeatherChanged& e) { changes.push_back(e); })) {}
};

}  // namespace

TEST_CASE("A character enters the world under a clear sky until the realm says otherwise", "[weather]") {
    GameHarness h;
    h.enterWorld();
    CHECK(h.game().weather().current().kind == WeatherKind::Clear);
    CHECK(h.game().weather().current().intensity == 0.0f);
}

TEST_CASE("Heavy rain from the realm is rain, and the client and the interface are told", "[weather]") {
    GameHarness h;
    h.enterWorld();
    Heard heard(h);

    h.receive(Opcode::SMSG_WEATHER, weather(5, 0.8f));
    h.pump();

    const auto& now = h.game().weather().current();
    CHECK(now.kind == WeatherKind::Rain);
    CHECK(now.intensity == Approx(0.8f));
    CHECK(now.realmState == 5u);
    CHECK_FALSE(now.abrupt);

    REQUIRE(heard.changes.size() == 1);
    CHECK(heard.changes[0].kind == WeatherKind::Rain);
    CHECK(heard.changes[0].previous == WeatherKind::Clear);
    CHECK(heard.changes[0].intensity == Approx(0.8f));

    const auto fired = h.addonEvents("WEATHER_CHANGED");
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0].args.size() == 2);
    CHECK(fired[0].args[0] == "5");
}

TEST_CASE("Every grade of a kind reads as that kind", "[weather]") {
    GameHarness h;
    h.enterWorld();
    const struct {
        uint32_t state;
        WeatherKind kind;
    } cases[] = {
        {1, WeatherKind::Fog},          {2, WeatherKind::Rain},       {3, WeatherKind::Rain},
        {4, WeatherKind::Rain},         {6, WeatherKind::Snow},       {7, WeatherKind::Snow},
        {8, WeatherKind::Snow},         {22, WeatherKind::Sandstorm}, {41, WeatherKind::Sandstorm},
        {42, WeatherKind::Sandstorm},   {86, WeatherKind::Thunderstorm},
        {90, WeatherKind::BlackRain},   {106, WeatherKind::BlackSnow}, {0, WeatherKind::Clear},
    };
    for (const auto& c : cases) {
        h.receive(Opcode::SMSG_WEATHER, weather(c.state, 0.5f));
        h.pump();
        INFO("realm weather " << c.state);
        CHECK(h.game().weather().current().kind == c.kind);
    }
}

TEST_CASE("Weather the realm asks to arrive at once says so", "[weather]") {
    GameHarness h;
    h.enterWorld();
    Heard heard(h);

    h.receive(Opcode::SMSG_WEATHER, weather(42, 1.0f, true));
    h.pump();

    REQUIRE(heard.changes.size() == 1);
    CHECK(heard.changes[0].kind == WeatherKind::Sandstorm);
    CHECK(heard.changes[0].abrupt);
    CHECK(h.game().weather().current().abrupt);
}

TEST_CASE("The weather turning from one kind to another names both", "[weather]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_WEATHER, weather(4, 0.5f));
    h.pump();
    Heard heard(h);

    h.receive(Opcode::SMSG_WEATHER, weather(7, 0.6f));
    h.pump();

    REQUIRE(heard.changes.size() == 1);
    CHECK(heard.changes[0].previous == WeatherKind::Rain);
    CHECK(heard.changes[0].kind == WeatherKind::Snow);
}

TEST_CASE("A weather number the client does not know is drawn as clear", "[weather]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_WEATHER, weather(5, 0.8f));
    h.pump();

    h.receive(Opcode::SMSG_WEATHER, weather(77, 0.8f));
    h.pump();

    CHECK(h.game().weather().current().kind == WeatherKind::Clear);
    CHECK(h.game().weather().current().realmState == 77u);
}

TEST_CASE("An intensity outside 0-1 is held to it", "[weather]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_WEATHER, weather(5, 3.0f));
    h.pump();
    CHECK(h.game().weather().current().intensity == Approx(1.0f));
}

TEST_CASE("The next character does not inherit the last one's rain", "[weather]") {
    GameHarness h;
    h.enterWorld();
    h.receive(Opcode::SMSG_WEATHER, weather(5, 0.9f));
    h.pump();
    REQUIRE(h.game().weather().current().kind == WeatherKind::Rain);

    PlayerSpec next;
    next.guid = 0x11;
    next.name = "Second";
    h.relogAs(next);

    CHECK(h.game().weather().current().kind == WeatherKind::Clear);
    CHECK(h.game().weather().current().intensity == 0.0f);
}

TEST_CASE("A 1.12 realm's storm is a sandstorm, where a WotLK realm's 3 is light rain", "[weather]") {
    CHECK(decodeWeather(3, WeatherNumbering::Classic) == WeatherKind::Sandstorm);
    CHECK(decodeWeather(1, WeatherNumbering::Classic) == WeatherKind::Rain);
    CHECK(decodeWeather(2, WeatherNumbering::Classic) == WeatherKind::Snow);
    CHECK(decodeWeather(3, WeatherNumbering::WeatherState) == WeatherKind::Rain);
    CHECK_FALSE(decodeWeather(42, WeatherNumbering::Classic).has_value());
}
