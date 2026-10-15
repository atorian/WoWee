#include <catch_amalgamated.hpp>
#include "core/world_load_progress.hpp"

using namespace wowee::core;

TEST_CASE("world warmup waits for server burst convergence and a floor near spawn", "[loading]") {
    WorldWarmupProgress warmup;
    for (int frame = 0; frame < 20; ++frame)
        CHECK(warmup.observe(1.0f, true, true, 20, true).result == WorldLoadResult::Pending);
    CHECK(warmup.observe(2.0f, true, true, 20, true).result == WorldLoadResult::Ready);
}

TEST_CASE("new spawn work interrupts the consecutive idle window", "[loading]") {
    WorldWarmupProgress warmup;
    for (int frame = 0; frame < 4; ++frame)
        CHECK(warmup.observe(3.0f, true, true, 20, true).result == WorldLoadResult::Pending);
    CHECK(warmup.observe(3.0f, false, true, 20, true).result == WorldLoadResult::Pending);
    for (int frame = 0; frame < 4; ++frame)
        CHECK(warmup.observe(3.1f, true, true, 20, true).result == WorldLoadResult::Pending);
    CHECK(warmup.observe(3.2f, true, true, 20, true).result == WorldLoadResult::Ready);
}

TEST_CASE("terrain far below a city does not immediately release the loading screen", "[loading]") {
    WorldWarmupProgress warmup;
    for (int frame = 0; frame < 10; ++frame)
        CHECK(warmup.observe(4.9f, true, false, 4, true).result == WorldLoadResult::Pending);
    const auto fallback = warmup.observe(5.0f, true, false, 4, true);
    CHECK(fallback.result == WorldLoadResult::Ready);
    CHECK(fallback.reason.find("fallback") != std::string::npos);
}

TEST_CASE("warmup hard cap retains its visible warning and still waits for uploads", "[loading]") {
    WorldWarmupProgress warmup;
    CHECK(warmup.observe(25.0f, false, false, 0, false).result == WorldLoadResult::Pending);
    const auto degraded = warmup.observe(25.1f, false, false, 0, true);
    CHECK(degraded.result == WorldLoadResult::Ready);
    CHECK(degraded.reason.find("ground is not ready") != std::string::npos);
}

TEST_CASE("normal world readiness does not expose buffers still being uploaded", "[loading]") {
    WorldWarmupProgress warmup;
    for (int frame = 0; frame < 10; ++frame)
        CHECK(warmup.observe(3, true, true, 20, false).result == WorldLoadResult::Pending);
    CHECK(warmup.observe(3.1f, true, true, 20, true).result == WorldLoadResult::Ready);
}

TEST_CASE("terrain streaming waits until the last ready tile has finalized", "[loading]") {
    WorldTileProgress terrain;
    CHECK(terrain.observe(0, 1, 80).result == WorldLoadResult::Pending);
    CHECK(terrain.observe(2, 80, 1).result == WorldLoadResult::Pending);
    CHECK(terrain.observe(2.1f, 81, 0).result == WorldLoadResult::Ready);
}

TEST_CASE("terrain preload distinguishes a stalled queue from a queue still making progress", "[loading]") {
    WorldTileProgress stalled, moving;
    stalled.observe(0, 1, 80);
    const auto fallback = stalled.observe(10.1f, 1, 80);
    CHECK(fallback.result == WorldLoadResult::Ready);
    CHECK(fallback.reason.find("stalled") != std::string::npos);
    moving.observe(0, 1, 80);
    CHECK(moving.observe(9, 20, 61).result == WorldLoadResult::Pending);
    CHECK(moving.observe(11, 21, 60).result == WorldLoadResult::Pending);
}

TEST_CASE("terrain preload hard cap does not become an unlimited loading screen", "[loading]") {
    WorldTileProgress terrain;
    for (int second = 0; second <= 60; ++second)
        CHECK(terrain.observe(static_cast<float>(second), second, 100 - second).result == WorldLoadResult::Pending);
    const auto fallback = terrain.observe(60.1f, 61, 39);
    CHECK(fallback.result == WorldLoadResult::Ready);
    CHECK(fallback.reason.find("timed out") != std::string::npos);
}
