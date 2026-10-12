/// Sizing each frame's share of ray-tracing geometry from measured cost: the
/// queue should drain as fast as a 60 fps frame allows, back off at once when
/// a frame runs over, and still move when there is no room at all.

#include <catch_amalgamated.hpp>

#include "rendering/rt_build_budget.hpp"

using wowee::rendering::RtBuildBudget;

namespace {
// A GPU that builds `trisPerMs` triangles a millisecond, in a frame whose
// other work costs `restMs`; returns the frame's GPU time.
double run(RtBuildBudget& b, double trisPerMs, double restMs, int frames) {
    double frameMs = 0;
    for (int i = 0; i < frames; ++i) {
        const uint64_t tris = b.budget();
        const double buildMs = 0.3 + double(tris) / trisPerMs;
        frameMs = restMs + buildMs;
        b.observe({tris, buildMs, frameMs});
    }
    return frameMs;
}
}  // namespace

TEST_CASE("the share settles where the frame meets 60 fps", "[rt]") {
    RtBuildBudget b;
    const double frameMs = run(b, 20000.0, 8.0, 60);
    CHECK(frameMs <= 1000.0 / 60.0);
    CHECK(frameMs > 1000.0 / 60.0 - 2.5);   // and uses the room it has
    CHECK(b.budget() > 100000);
}

TEST_CASE("a frame over target cuts the share to the trickle at once", "[rt]") {
    RtBuildBudget b;
    run(b, 20000.0, 4.0, 30);
    REQUIRE(b.budget() > RtBuildBudget::kMinTriangles);
    b.observe({b.budget(), 2.0, 40.0});   // Dalaran: the rest of the frame alone is over
    CHECK(b.budget() == RtBuildBudget::kMinTriangles);
}

TEST_CASE("the share grows at most twofold a frame", "[rt]") {
    RtBuildBudget b;
    const uint64_t before = b.budget();
    b.observe({0, 0.3, 1.0});   // an idle, nearly empty frame
    CHECK(b.budget() <= before * 2);
}

TEST_CASE("the cost per triangle follows the measurements", "[rt]") {
    RtBuildBudget slow, fast;
    run(slow, 2000.0, 6.0, 40);
    run(fast, 50000.0, 6.0, 40);
    CHECK(slow.msPerTriangle() > fast.msPerTriangle() * 10);
    CHECK(slow.budget() < fast.budget());
}

TEST_CASE("the share stays within its bounds", "[rt]") {
    RtBuildBudget b;
    for (int i = 0; i < 100; ++i) b.observe({b.budget(), 0.31, 0.5});   // impossibly cheap
    CHECK(b.budget() <= RtBuildBudget::kMaxTriangles);
    b.observe({b.budget(), 500.0, 900.0});
    CHECK(b.budget() >= RtBuildBudget::kMinTriangles);
}
