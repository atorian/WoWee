// The weather as the renderer draws it (WeatherBlend): what each kind is made
// of, and how one weather comes over into the next.
#include <catch_amalgamated.hpp>

#include "rendering/weather_mix.hpp"

using Catch::Approx;
using wowee::pipeline::WeatherKind;
using wowee::rendering::WeatherBlend;
using wowee::rendering::WeatherTarget;

namespace {

constexpr float kFrame = 1.0f / 60.0f;

void run(WeatherBlend& blend, const WeatherTarget& target, float seconds) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += kFrame) blend.update(kFrame, target);
}

}  // namespace

TEST_CASE("The first weather a renderer is given is there at once", "[weather]") {
    WeatherBlend blend;
    blend.update(kFrame, {WeatherKind::Rain, 0.7f, false});
    CHECK(blend.mix().rain == Approx(0.7f));
    CHECK(blend.mix().strength() == Approx(0.7f));
}

TEST_CASE("A shower comes over in the transition time, not at once", "[weather]") {
    WeatherBlend blend;
    blend.setTransitionSeconds(10.0f);
    blend.update(kFrame, {});

    run(blend, {WeatherKind::Rain, 1.0f, false}, 5.0f);
    CHECK(blend.mix().rain == Approx(0.5f).margin(0.02f));

    run(blend, {WeatherKind::Rain, 1.0f, false}, 5.5f);
    CHECK(blend.mix().rain == Approx(1.0f));
}

TEST_CASE("Rain turning to snow fades out while the snow comes in", "[weather]") {
    WeatherBlend blend;
    blend.setTransitionSeconds(8.0f);
    blend.update(kFrame, {WeatherKind::Rain, 0.8f, false});

    run(blend, {WeatherKind::Snow, 0.8f, false}, 4.0f);
    CHECK(blend.mix().rain > 0.2f);
    CHECK(blend.mix().snow > 0.2f);

    run(blend, {WeatherKind::Snow, 0.8f, false}, 5.0f);
    CHECK(blend.mix().rain == 0.0f);
    CHECK(blend.mix().snow == Approx(0.8f));
}

TEST_CASE("Weather the realm asks to arrive at once does", "[weather]") {
    WeatherBlend blend;
    blend.update(kFrame, {});
    blend.update(kFrame, {WeatherKind::Sandstorm, 0.9f, true});
    CHECK(blend.mix().sand == Approx(0.9f));
}

TEST_CASE("A thunderstorm is rain with lightning over it, black rain is dark rain", "[weather]") {
    WeatherBlend storm;
    storm.update(kFrame, {WeatherKind::Thunderstorm, 0.6f, false});
    CHECK(storm.mix().rain == Approx(0.6f));
    CHECK(storm.mix().thunder == Approx(0.6f));

    WeatherBlend black;
    black.update(kFrame, {WeatherKind::BlackRain, 0.6f, false});
    CHECK(black.mix().rain == Approx(0.6f));
    CHECK(black.mix().dark == Approx(1.0f));
    CHECK(black.mix().thunder == 0.0f);
}

TEST_CASE("Black rain stays black as it clears", "[weather]") {
    WeatherBlend blend;
    blend.setTransitionSeconds(4.0f);
    blend.update(kFrame, {WeatherKind::BlackRain, 1.0f, false});
    run(blend, {}, 2.0f);
    CHECK(blend.mix().rain == Approx(0.5f).margin(0.02f));
    CHECK(blend.mix().dark == Approx(1.0f));
}

TEST_CASE("Fog weather is mist and nothing falling", "[weather]") {
    WeatherBlend blend;
    blend.update(kFrame, {WeatherKind::Fog, 0.5f, false});
    CHECK(blend.mix().mist == Approx(0.5f));
    CHECK(blend.mix().rain == 0.0f);
    CHECK(blend.mix().snow == 0.0f);
    CHECK(blend.mix().strength() == Approx(0.5f));
}
