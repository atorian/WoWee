// What the asset manager's file cache keeps. A texture is read to be decoded
// and uploaded, and the GPU's texture cache holds it from then on: its raw
// bytes kept here as well were a second copy of every texture streamed, in
// system memory, up to half of what the machine had free. A file read for its
// own sake - a model, a map tile - is kept, for the next read of it.
#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

/// A 4x4 BLP2 holding one DXT1 block.
std::vector<uint8_t> tinyDxt1Blp() {
    constexpr size_t kHeader = 1172;
    std::vector<uint8_t> b(kHeader + 8, 0u);
    std::memcpy(b.data(), "BLP2", 4);
    const auto u32 = [&b](size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, sizeof(v)); };
    u32(4, 1);           // version
    b[8] = 2;            // compression: DXT
    b[9] = 0;            // alpha depth: none, so DXT1
    u32(12, 4);          // width
    u32(16, 4);          // height
    u32(20, kHeader);    // mip 0 offset
    u32(84, 8);          // mip 0 size
    // One block: both endpoints white.
    b[kHeader + 0] = 0xFF;
    b[kHeader + 1] = 0xFF;
    b[kHeader + 2] = 0xFF;
    b[kHeader + 3] = 0xFF;
    return b;
}

} // namespace

TEST_CASE("a texture read for the GPU leaves no copy of its file in the file cache", "[assets]") {
    const auto root = wowee::test::makeDataRoot("wowee_asset_file_cache", {
        {"world\\generic\\stone.blp", tinyDxt1Blp()},
        {"world\\generic\\crate.m2", std::vector<uint8_t>(64, 7)},
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));

    const auto image = assets.loadTexture("World\\Generic\\Stone.blp");
    REQUIRE(image.isValid());
    CHECK(assets.fileCacheStats().entries == 0);
    CHECK(assets.fileCacheStats().bytes == 0);

    // A model file is read for its own sake and kept for the next read.
    CHECK(assets.readFile("World\\Generic\\crate.m2").size() == 64);
    CHECK(assets.fileCacheStats().entries == 1);
    CHECK(assets.fileCacheStats().bytes == 64);
}

TEST_CASE("the file cache lets go of the file read longest ago, not the one kept longest", "[assets]") {
    const auto root = wowee::test::makeDataRoot("wowee_asset_file_cache_lru", {
        {"world\\a.m2", std::vector<uint8_t>(100, 1)},
        {"world\\b.m2", std::vector<uint8_t>(100, 2)},
        {"world\\c.m2", std::vector<uint8_t>(100, 3)},
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    assets.setFileCacheBudget(250);

    REQUIRE(assets.readFile("World\\a.m2").size() == 100);
    REQUIRE(assets.readFile("World\\b.m2").size() == 100);
    // A read again: now it is B that was read longest ago.
    REQUIRE(assets.readFile("World\\a.m2").size() == 100);
    // C does not fit beside both, so one of them goes.
    REQUIRE(assets.readFile("World\\c.m2").size() == 100);
    CHECK(assets.fileCacheStats().entries == 2);

    const size_t hitsBefore = assets.getFileCacheHits();
    CHECK(assets.readFile("World\\a.m2")[0] == 1);
    CHECK(assets.getFileCacheHits() == hitsBefore + 1);  // still kept
    CHECK(assets.readFile("World\\b.m2")[0] == 2);
    CHECK(assets.getFileCacheHits() == hitsBefore + 1);  // let go, read anew
}

TEST_CASE("a file kept in the cache is shared out, not copied for every reader", "[assets]") {
    const auto root = wowee::test::makeDataRoot("wowee_asset_file_cache_shared", {
        {"world\\crate.m2", std::vector<uint8_t>(64, 7)},
    });
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));

    const auto first = assets.readFileShared("World\\Crate.m2");
    const auto second = assets.readFileShared("world/crate.m2");
    REQUIRE(first);
    CHECK(first->size() == 64);
    CHECK(first.get() == second.get());

    // Still the reader's after the cache forgets it.
    assets.clearCache();
    CHECK(first->size() == 64);
    CHECK((*first)[0] == 7);

    CHECK_FALSE(assets.readFileShared("World\\Missing.m2"));
}
