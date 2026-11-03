// The unit model cache: a model's file is parsed once, off the asking thread
// when the frame asks, and everyone who asks for it is handed that one parse.
// The target portrait parsed the target's model again on every click, on the
// frame thread, while the world held it parsed already.
#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/m2_model_cache.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using wowee::pipeline::M2ModelCache;

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, sizeof(v)); }

/// A WotLK M2 of one triangle: the 304-byte header and three vertices.
std::vector<uint8_t> triangleM2() {
    std::vector<uint8_t> b(304 + 3 * 48, 0u);
    std::memcpy(b.data(), "MD20", 4);
    put32(b, 4, 264);   // version
    put32(b, 60, 3);    // vertices
    put32(b, 64, 304);
    const float x[3] = {0.0f, 1.0f, 0.0f};
    const float y[3] = {0.0f, 0.0f, 1.0f};
    for (int v = 0; v < 3; ++v) {
        std::memcpy(b.data() + 304 + 48 * v, &x[v], 4);
        std::memcpy(b.data() + 304 + 48 * v + 4, &y[v], 4);
    }
    return b;
}

/// Its .skin: the three vertices looked up in order, one triangle of them.
std::vector<uint8_t> triangleSkin() {
    std::vector<uint8_t> b(48 + 12, 0u);
    std::memcpy(b.data(), "SKIN", 4);
    put32(b, 4, 3);    // lookup
    put32(b, 8, 48);
    put32(b, 12, 3);   // triangle indices
    put32(b, 16, 54);
    const uint16_t lookup[3] = {0, 1, 2};
    std::memcpy(b.data() + 48, lookup, sizeof(lookup));
    std::memcpy(b.data() + 54, lookup, sizeof(lookup));
    return b;
}

/// What the frame is handed once the worker is done; Loading until then.
M2ModelCache::Request waitFor(M2ModelCache& cache, const char* path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto asked = cache.request(path);
    while (asked.state == M2ModelCache::State::Loading && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        asked = cache.request(path);
    }
    return asked;
}

} // namespace

TEST_CASE("a unit model is parsed once and every asker is handed that parse", "[m2][cache]") {
    const auto root = wowee::test::makeDataRoot("wowee_m2_model_cache_once", {
        {"creature\\wolf\\wolf.m2", triangleM2()},
        {"creature\\wolf\\wolf00.skin", triangleSkin()},
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    M2ModelCache cache(assets, size_t{64} << 20);

    const auto asked = waitFor(cache, "Creature\\Wolf\\Wolf.m2");
    REQUIRE(asked.state == M2ModelCache::State::Ready);
    // Parsed with its skin: a mesh to draw.
    CHECK(asked.model->vertices.size() == 3);
    CHECK(asked.model->indices.size() == 3);
    // The world's worker and the frame, under either spelling of the path,
    // get the very same parse rather than another one.
    CHECK(cache.load("creature/wolf/wolf.m2") == asked.model);
    CHECK(cache.request("CREATURE\\WOLF\\WOLF.M2").model == asked.model);
}

TEST_CASE("a unit model that is not there fails once and is not waited for", "[m2][cache]") {
    const auto root = wowee::test::makeDataRoot("wowee_m2_model_cache_missing", {
        {"creature\\wolf\\wolf.m2", triangleM2()},
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    M2ModelCache cache(assets, size_t{64} << 20);

    CHECK(waitFor(cache, "Creature\\Bear\\Bear.m2").state == M2ModelCache::State::Failed);
    CHECK(cache.load("Creature\\Bear\\Bear.m2") == nullptr);
}

TEST_CASE("unit models past the budget are let go, the least recently asked for first", "[m2][cache]") {
    const auto root = wowee::test::makeDataRoot("wowee_m2_model_cache_budget", {
        {"creature\\wolf\\wolf.m2", triangleM2()},
        {"creature\\wolf\\wolf00.skin", triangleSkin()},
        {"creature\\bear\\bear.m2", triangleM2()},
        {"creature\\bear\\bear00.skin", triangleSkin()},
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    // Room for one of them: a triangle's parse is a little under a kilobyte.
    M2ModelCache cache(assets, 1500);

    const auto wolf = cache.load("Creature\\Wolf\\Wolf.m2");
    REQUIRE(wolf);
    REQUIRE(cache.load("Creature\\Bear\\Bear.m2"));
    // The wolf went to make room: asked again, it is parsed again. Whoever
    // still holds the first parse keeps it.
    const auto again = cache.load("Creature\\Wolf\\Wolf.m2");
    REQUIRE(again);
    CHECK(again != wolf);
    CHECK(wolf->vertices.size() == 3);
}
