// What a benchmark run does with the frames it is told about: nothing is
// measured until the world has been in view and settled, the camera flies the
// route's loop once in the flight's time, and the report covers the flight
// and nothing before it.
#include <catch_amalgamated.hpp>

#include "core/benchmark_run.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using wowee::core::BenchmarkRoute;
using wowee::core::BenchmarkRun;
using wowee::core::cameraAlong;

namespace {

BenchmarkRoute square() {
    BenchmarkRoute route;
    route.name = "square";
    route.loop = {
        {{100.0f, 0.0f, 50.0f}, {0.0f, 0.0f, 0.0f}},
        {{0.0f, 100.0f, 50.0f}, {0.0f, 0.0f, 0.0f}},
        {{-100.0f, 0.0f, 50.0f}, {0.0f, 0.0f, 0.0f}},
        {{0.0f, -100.0f, 50.0f}, {0.0f, 0.0f, 0.0f}},
    };
    return route;
}

BenchmarkRun::Timing timing() {
    BenchmarkRun::Timing t;
    t.settleSeconds = 2.0f;
    t.maxSettleSeconds = 5.0f;
    t.flySeconds = 4.0f;
    return t;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("nothing is flown or measured until the world has been in view for the settle time",
          "[benchmark]") {
    BenchmarkRun run(square(), timing());
    // The loading screen.
    for (int i = 0; i < 100; ++i) run.frameEnded(0.1f, false, true);
    CHECK(run.phase() == BenchmarkRun::Phase::WaitingForWorld);
    CHECK_FALSE(run.camera());

    // In view, settling: 1.9 s.
    for (int i = 0; i < 19; ++i) run.frameEnded(0.1f, true, false);
    CHECK(run.phase() == BenchmarkRun::Phase::Settling);
    CHECK_FALSE(run.recording());

    run.frameEnded(0.1f, true, false);
    run.frameEnded(0.1f, true, false);
    CHECK(run.recording());
    REQUIRE(run.camera());
    CHECK(run.camera()->position.x == Catch::Approx(100.0f));
}

TEST_CASE("the flight waits for terrain to stream in, but not past the limit", "[benchmark]") {
    BenchmarkRun run(square(), timing());
    run.frameEnded(0.1f, true, true);
    for (int i = 0; i < 40; ++i) run.frameEnded(0.1f, true, true);
    CHECK(run.phase() == BenchmarkRun::Phase::Settling);
    for (int i = 0; i < 11; ++i) run.frameEnded(0.1f, true, true);
    CHECK(run.recording());
    // The report says the flight began with streaming still under way.
    bool said = false;
    for (const auto& line : run.report({})) said = said || contains(line, "still streaming");
    CHECK(said);
}

TEST_CASE("the camera passes through every waypoint, looking where each looks, and comes round",
          "[benchmark]") {
    const BenchmarkRoute route = square();
    for (size_t i = 0; i < route.loop.size(); ++i) {
        const auto camera = cameraAlong(route, static_cast<float>(i) / route.loop.size());
        CHECK(camera.position.x == Catch::Approx(route.loop[i].eye.x).margin(1e-3));
        CHECK(camera.position.y == Catch::Approx(route.loop[i].eye.y).margin(1e-3));
        CHECK(camera.position.z == Catch::Approx(route.loop[i].eye.z).margin(1e-3));
        // Aimed at the centre, below it.
        const glm::vec3 toCentre = route.loop[i].lookAt - route.loop[i].eye;
        const float yaw = std::atan2(toCentre.y, toCentre.x) * 180.0f / 3.14159265f;
        CHECK(std::remainder(camera.yawDegrees - yaw, 360.0f) == Catch::Approx(0.0f).margin(1e-2));
        CHECK(camera.pitchDegrees < 0.0f);
    }
    const auto end = cameraAlong(route, 1.0f);
    CHECK(end.position.x == Catch::Approx(route.loop.front().eye.x).margin(1e-3));
    CHECK(end.position.y == Catch::Approx(route.loop.front().eye.y).margin(1e-3));
}

TEST_CASE("the report covers the flight and ends it after the flight's time", "[benchmark]") {
    BenchmarkRun run(square(), timing());
    const auto frame = [&](float seconds) {
        if (run.recording()) {
            run.budget().noteStage("beginFrame", seconds * 500.0f);
            run.budget().frameFinished(seconds * 1000.0f);
            run.noteSceneCount("M2 instances", 1000);
        }
        run.frameEnded(seconds, true, false);
    };
    // Settling frames are slow and must not appear.
    for (int i = 0; i < 100 && !run.recording(); ++i) frame(0.1f);
    REQUIRE(run.recording());
    // Four seconds of flight: frames of 20 ms and one of 100.
    for (int i = 0; i < 195; ++i) frame(0.02f);
    frame(0.1f);
    while (run.phase() != BenchmarkRun::Phase::Finished) frame(0.02f);
    CHECK_FALSE(run.camera());

    const std::string summary = run.summary();
    CHECK(contains(summary, "route square"));
    CHECK(contains(summary, "p50 20.00"));
    CHECK(contains(summary, "max 100.00 ms"));
    CHECK(contains(summary, "over 33.3ms: 1 ("));

    const auto lines = run.report({"device: test"});
    bool device = false, stage = false, scene = false;
    for (const auto& line : lines) {
        device = device || line == "device: test";
        stage = stage || contains(line, "beginFrame:");
        scene = scene || contains(line, "M2 instances: 1000, 1000");
    }
    CHECK(device);
    CHECK(stage);
    CHECK(scene);
}

TEST_CASE("a report is written where it is asked for, under the route's name", "[benchmark]") {
    const auto dir = std::filesystem::temp_directory_path() / "wowee_benchmark_report";
    std::filesystem::remove_all(dir);
    const auto written = wowee::core::writeBenchmarkReport(dir, "square", {"one", "two"});
    REQUIRE(written);
    CHECK(written->parent_path() == dir);
    CHECK(written->filename().string().rfind("benchmark-square-", 0) == 0);
    std::ifstream in(*written);
    std::string first, second;
    std::getline(in, first);
    std::getline(in, second);
    CHECK(first == "one");
    CHECK(second == "two");
}
