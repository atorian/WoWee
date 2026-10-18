// What the weather leaves on the camera's lens (LensDrops), stepped frame by
// frame as the renderer steps it.
#include <catch_amalgamated.hpp>

#include "rendering/lens_drops.hpp"

#include <algorithm>

using wowee::rendering::LensDrop;
using wowee::rendering::LensDrops;
using wowee::rendering::LensInputs;

namespace {

constexpr float kFrame = 1.0f / 60.0f;
constexpr float kAspect = 16.0f / 9.0f;

void run(LensDrops& lens, const LensInputs& in, float seconds) {
    for (float t = 0.0f; t < seconds; t += kFrame) lens.update(kFrame, in, kAspect);
}

size_t count(const LensDrops& lens, LensDrop::Kind kind) {
    return static_cast<size_t>(std::count_if(lens.drops().begin(), lens.drops().end(),
                                             [kind](const LensDrop& d) { return d.kind == kind; }));
}

LensInputs rainInTheOpen() {
    LensInputs in;
    in.rain = 1.0f;
    in.exposure = 1.0f;
    in.facingFall = 1.0f;
    return in;
}

}  // namespace

TEST_CASE("Standing in the rain looking into it, drops gather on the lens", "[weather][lens]") {
    LensDrops lens;
    run(lens, rainInTheOpen(), 3.0f);
    CHECK(count(lens, LensDrop::Kind::Water) >= 10);
}

TEST_CASE("Looking away from the rain, or under a roof, the lens stays dry", "[weather][lens]") {
    LensInputs away = rainInTheOpen();
    away.facingFall = 0.0f;
    LensDrops looking;
    run(looking, away, 5.0f);
    CHECK(looking.drops().empty());

    LensInputs roofed = rainInTheOpen();
    roofed.exposure = 0.0f;
    LensDrops sheltered;
    run(sheltered, roofed, 5.0f);
    CHECK(sheltered.drops().empty());
}

TEST_CASE("Out of the rain the drops dry off, and faster under a roof", "[weather][lens]") {
    LensDrops open;
    LensDrops roofed;
    run(open, rainInTheOpen(), 3.0f);
    run(roofed, rainInTheOpen(), 3.0f);
    REQUIRE_FALSE(open.drops().empty());

    LensInputs stopped = rainInTheOpen();
    stopped.rain = 0.0f;
    LensInputs inside = stopped;
    inside.exposure = 0.0f;
    run(open, stopped, 2.5f);
    run(roofed, inside, 2.5f);
    CHECK(roofed.drops().size() < open.drops().size());

    run(open, stopped, 12.0f);
    CHECK(open.drops().empty());
}

TEST_CASE("Big drops run down the lens and leave beads behind", "[weather][lens]") {
    LensDrops lens;
    // Long enough for the drops to merge into ones big enough to run.
    LensInputs in = rainInTheOpen();
    bool sawRunning = false;
    for (int frame = 0; frame < 60 * 10 && !sawRunning; ++frame) {
        lens.update(kFrame, in, kAspect);
        sawRunning = std::any_of(lens.drops().begin(), lens.drops().end(),
                                 [](const LensDrop& d) { return d.running(); });
    }
    REQUIRE(sawRunning);

    const auto runner = std::find_if(lens.drops().begin(), lens.drops().end(),
                                     [](const LensDrop& d) { return d.running(); });
    const float seed = runner->seed;
    const float startY = runner->pos.y;
    in.rain = 0.0f;  // nothing new merging into it
    lens.update(0.25f, in, kAspect);
    const auto later = std::find_if(lens.drops().begin(), lens.drops().end(),
                                    [seed](const LensDrop& d) { return d.seed == seed; });
    if (later != lens.drops().end()) {
        CHECK(later->pos.y > startY);
    } else {
        SUCCEED("ran off the bottom of the lens");
    }
}

TEST_CASE("Coming up out of water, water runs down the lens", "[weather][lens]") {
    LensDrops lens;
    LensInputs dry;
    LensInputs under;
    under.underwater = true;
    run(lens, under, 1.0f);
    CHECK(lens.drops().empty());

    lens.update(kFrame, dry, kAspect);
    CHECK(count(lens, LensDrop::Kind::Water) >= 20);
    CHECK(std::all_of(lens.drops().begin(), lens.drops().end(), [](const LensDrop& d) { return d.running(); }));
}

TEST_CASE("Snow sticks to the lens, melts to water and frosts the edges", "[weather][lens]") {
    LensDrops lens;
    LensInputs snow;
    snow.snow = 1.0f;
    snow.exposure = 1.0f;
    snow.facingFall = 1.0f;
    run(lens, snow, 3.0f);
    CHECK(count(lens, LensDrop::Kind::Flake) > 0);
    // The frost takes its time: some in a few seconds, more the longer it snows.
    const float early = lens.frost();
    CHECK(early > 0.0f);
    run(lens, snow, 10.0f);
    CHECK(lens.frost() > early + 0.2f);

    LensInputs stopped = snow;
    stopped.snow = 0.0f;
    run(lens, stopped, 4.5f);
    CHECK(count(lens, LensDrop::Kind::Flake) == 0);
    CHECK(count(lens, LensDrop::Kind::Water) > 0);

    run(lens, stopped, 30.0f);
    CHECK(lens.frost() == 0.0f);
}

TEST_CASE("Sand is driven across the lens the way the wind blows and leaves a film", "[weather][lens]") {
    LensDrops lens;
    LensInputs sand;
    sand.sand = 1.0f;
    sand.exposure = 1.0f;
    sand.facingWind = 1.0f;
    sand.windOnScreen = {1.5f, 0.1f};
    run(lens, sand, 2.0f);

    REQUIRE(count(lens, LensDrop::Kind::Grit) > 0);
    for (const LensDrop& d : lens.drops()) {
        if (d.kind == LensDrop::Kind::Grit) CHECK(d.velocity.x > 0.0f);
    }
    CHECK(lens.dust() > 0.1f);
    CHECK(count(lens, LensDrop::Kind::Water) == 0);
}

TEST_CASE("The same weather leaves the same lens every run", "[weather][lens]") {
    LensDrops a(7u);
    LensDrops b(7u);
    run(a, rainInTheOpen(), 2.0f);
    run(b, rainInTheOpen(), 2.0f);
    REQUIRE(a.drops().size() == b.drops().size());
    for (size_t i = 0; i < a.drops().size(); ++i) CHECK(a.drops()[i].pos == b.drops()[i].pos);
}
