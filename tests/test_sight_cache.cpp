// Line of sight for nameplates and chat bubbles (ui/sight_cache.hpp): whether
// a wall stands between the camera and a unit, as a collision query the cache
// runs as few times a frame as it can.
//
// The query here is a stand-in that counts its calls and answers what the
// test says the world is, so each case reads how many queries a frame cost
// and what a plate is told.
#include <catch_amalgamated.hpp>

#include "ui/sight_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

using wowee::ui::SightCache;

namespace {

struct World {
    bool wall = false;
    int queries = 0;
    /// How long one query takes, for the time budget.
    float costMs = 0.0f;
    bool operator()(const glm::vec3&, const glm::vec3&) {
        ++queries;
        if (costMs > 0.0f) {
            // In whole microseconds: a float time point since the clock's
            // epoch has no precision left at this scale.
            const auto until = std::chrono::steady_clock::now() +
                std::chrono::microseconds(static_cast<long long>(costMs * 1000.0f));
            while (std::chrono::steady_clock::now() < until) {}
        }
        return wall;
    }
};

glm::vec3 unitAt(uint64_t guid) {
    return {static_cast<float>(guid) * 3.0f, 10.0f, 2.0f};
}

/// One frame of plates: every unit asked about, then the refresh.
std::vector<bool> frame(SightCache& sight, World& world, uint64_t units, const glm::vec3& camera) {
    std::vector<bool> blocked(units + 1, false);
    for (uint64_t g = 1; g <= units; ++g) blocked[g] = sight.blocked(g, camera, unitAt(g));
    sight.refresh(world);
    return blocked;
}

} // namespace

TEST_CASE("a unit seen for the first time is out of sight until it is answered, the next frame",
          "[ui][sight]") {
    SightCache sight;
    World world;
    // Nothing in the way - but nothing has been asked yet, and a name drawn
    // through a wall for a frame is the fault this exists to stop.
    CHECK(sight.blocked(1, glm::vec3(0.0f), unitAt(1)));
    sight.refresh(world);
    CHECK(world.queries == 1);
    CHECK_FALSE(sight.blocked(1, glm::vec3(0.0f), unitAt(1)));
}

TEST_CASE("a unit nothing has moved around is not queried again every frame", "[ui][sight]") {
    SightCache sight;
    World world;
    const glm::vec3 camera(0.0f);
    frame(sight, world, 1, camera);
    world.queries = 0;
    for (int f = 0; f < SightCache::kMaxAgeFrames - 2; ++f) frame(sight, world, 1, camera);
    CHECK(world.queries == 0);
}

TEST_CASE("a moving camera refreshes a crowd a few a frame, and every plate in turn",
          "[ui][sight]") {
    // Dalaran: a hundred players in range, and the camera moving every frame.
    // Re-querying all of them as the camera moved put a hundred collision
    // queries into one frame, frame after frame.
    constexpr uint64_t kUnits = 100;
    SightCache sight;
    World world;
    for (int f = 0; f < 20; ++f) frame(sight, world, kUnits, glm::vec3(0.0f));

    // A wall goes up between the camera and all of them.
    world.wall = true;
    world.queries = 0;
    std::vector<bool> told(kUnits + 1, false);
    int worstFrame = 0;
    int frames = 0;
    for (; frames < 40; ++frames) {
        const int before = world.queries;
        const auto blocked = frame(sight, world, kUnits,
                                   glm::vec3(static_cast<float>(frames + 1), 0.0f, 0.0f));
        for (uint64_t g = 1; g <= kUnits; ++g) told[g] = told[g] || blocked[g];
        worstFrame = std::max(worstFrame, world.queries - before);
        if (std::all_of(told.begin() + 1, told.end(), [](bool b) { return b; })) break;
    }
    CHECK(worstFrame <= static_cast<int>(SightCache::kRefreshesPerFrame));
    // Everyone learned of the wall, and soon: no plate is left behind by the
    // ones nearer the front of the list.
    CHECK(std::all_of(told.begin() + 1, told.end(), [](bool b) { return b; }));
    CHECK(frames <= static_cast<int>((kUnits + SightCache::kRefreshesPerFrame - 1) /
                                     SightCache::kRefreshesPerFrame) + 2);
}

TEST_CASE("slow queries are held to the frame's time, but one still runs", "[ui][sight]") {
    // A query against Dalaran costs far more than one against a field, so a
    // count alone does not bound the frame.
    constexpr uint64_t kUnits = 20;
    SightCache sight;
    World world;
    world.costMs = SightCache::kRefreshBudgetMs * 0.6f;
    frame(sight, world, kUnits, glm::vec3(0.0f));
    CHECK(world.queries == 2);

    world.costMs = SightCache::kRefreshBudgetMs * 3.0f;
    world.queries = 0;
    frame(sight, world, kUnits, glm::vec3(0.0f));
    CHECK(world.queries == 1);
}

TEST_CASE("a crowd larger than the cache keeps the answers it is using", "[ui][sight]") {
    // More units pass through a city in an evening than the cache holds.
    // Making room by forgetting everything put every plate on screen back in
    // the line at once, hidden until its turn came round again.
    SightCache sight;
    World world;
    constexpr uint64_t kOnScreen = 10;
    for (int f = 0; f < 5; ++f) frame(sight, world, kOnScreen, glm::vec3(0.0f));
    for (uint64_t passer = 1000; passer < 3500; ++passer) {
        sight.blocked(passer, glm::vec3(0.0f), unitAt(passer));
        for (uint64_t g = 1; g <= kOnScreen; ++g) {
            REQUIRE_FALSE(sight.blocked(g, glm::vec3(0.0f), unitAt(g)));
        }
        sight.refresh(world);
    }
}
