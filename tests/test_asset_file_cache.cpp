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
#include <future>
#include <chrono>
#include <thread>

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


namespace {
class CountingTextureLayer final : public wowee::pipeline::ArchiveLayer {
public:
    mutable std::atomic<size_t> reads{0};
    std::vector<uint8_t> bytes = tinyDxt1Blp();
    bool contains(const std::string& path) const override {
        return path == "world\\shared.blp";
    }
    std::vector<uint8_t> read(const std::string&) const override {
        ++reads;
        return bytes;
    }
};
}

TEST_CASE("texture preparation reuses source reads across tiles and output forms", "[assets][texture-preparation]") {
    const auto root = wowee::test::makeDataRoot("wowee_texture_preparation", {});
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    auto layer = std::make_shared<CountingTextureLayer>();
    assets.setArchiveLayer(layer);
    const auto stored = assets.loadTexture("World/Shared.blp", true);
    const auto repeated = assets.loadTexture("world\\shared.blp", true);
    const auto pixels = assets.loadTexture("WORLD/SHARED.BLP", false);
    REQUIRE(stored.isValid());
    REQUIRE(repeated.isValid());
    REQUIRE(pixels.isValid());
    CHECK(pixels.data.size() == 4 * 4 * 4);
    CHECK(pixels.data[0] == 255);
    CHECK(layer->reads.load() == 1);
}


TEST_CASE("shared texture output survives invalidation and never exposes another output form", "[assets][texture-preparation]") {
    const auto root = wowee::test::makeDataRoot("wowee_texture_shared", {});
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    auto white = std::make_shared<CountingTextureLayer>();
    assets.setArchiveLayer(white);
    const auto first = assets.loadTextureShared("World/Shared.blp", true);
    const auto repeated = assets.loadTextureShared("world\\shared.blp", true);
    CHECK(first == repeated);
    const auto rgba = assets.loadTextureShared("world/shared.blp", false);
    REQUIRE(rgba->image.data.size() == 64);
    CHECK(rgba->image.data[0] == 255);
    CHECK(white->reads == 1);
    auto red = std::make_shared<CountingTextureLayer>();
    red->bytes[1172] = red->bytes[1174] = 0;
    red->bytes[1173] = red->bytes[1175] = 0xf8;
    assets.setArchiveLayer(red);
    const auto fresh = assets.loadTextureShared("world/shared.blp", false);
    REQUIRE(fresh->image.data.size() == 64);
    CHECK(fresh->image.data[0] == 255);
    CHECK(fresh->image.data[1] == 0);
    CHECK(rgba->image.data[1] == 255);
    CHECK(first->generation != fresh->generation);
    CHECK(red->reads == 1);
}

namespace {
class BlockingTextureLayer final : public wowee::pipeline::ArchiveLayer {
public:
    mutable std::atomic<size_t> reads{0};
    mutable std::promise<void> entered;
    std::shared_future<void> release;
    explicit BlockingTextureLayer(std::shared_future<void> gate) : release(std::move(gate)) {}
    bool contains(const std::string& path) const override { return path == "world\\shared.blp"; }
    std::vector<uint8_t> read(const std::string&) const override {
        if (++reads == 1) entered.set_value();
        release.wait();
        return tinyDxt1Blp();
    }
};
}

TEST_CASE("concurrent tile texture requests join one read without blocking a new asset generation", "[assets][texture-preparation]") {
    using namespace std::chrono_literals;
    const auto root = wowee::test::makeDataRoot("wowee_texture_flight", {});
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    std::promise<void> release;
    auto layer = std::make_shared<BlockingTextureLayer>(release.get_future().share());
    assets.setArchiveLayer(layer);
    auto a = std::async(std::launch::async, [&] { return assets.loadTextureShared("world/shared.blp", true); });
    layer->entered.get_future().wait();
    auto b = std::async(std::launch::async, [&] { return assets.loadTextureShared("WORLD/SHARED.BLP", true); });
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!assets.texturePreparationStats().joined && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    const bool joined = assets.texturePreparationStats().joined != 0;
    auto replacement = std::make_shared<CountingTextureLayer>();
    assets.setArchiveLayer(replacement);
    auto fresh = assets.loadTextureShared("world/shared.blp", true);
    release.set_value();
    const auto first = a.get(), second = b.get();
    CHECK(joined);
    CHECK(first == second);
    CHECK(first != fresh);
    CHECK(layer->reads == 1);
    CHECK(replacement->reads == 1);
    CHECK(assets.loadTextureShared("world/shared.blp", true) == fresh);
}

TEST_CASE("failed texture preparation is shared until asset invalidation", "[assets][texture-preparation]") {
    const auto root = wowee::test::makeDataRoot("wowee_texture_failure", {});
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    auto broken = std::make_shared<CountingTextureLayer>();
    broken->bytes = {0, 1, 2, 3};
    assets.setArchiveLayer(broken);
    CHECK_FALSE(assets.loadTextureShared("world/shared.blp", true)->image.isValid());
    CHECK_FALSE(assets.loadTextureShared("world/shared.blp", true)->image.isValid());
    CHECK(broken->reads == 1);
    assets.setArchiveLayer(std::make_shared<CountingTextureLayer>());
    CHECK(assets.loadTextureShared("world/shared.blp", true)->image.isValid());
}


TEST_CASE("mutable texture copies can change alpha without changing shared source facts", "[assets][texture-preparation]") {
    const auto root = wowee::test::makeDataRoot("wowee_texture_mutable", {});
    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(root.string()));
    assets.setArchiveLayer(std::make_shared<CountingTextureLayer>());
    const auto source = assets.loadTextureShared("world/shared.blp");
    REQUIRE(source->image.isValid());
    CHECK_FALSE(source->image.hasTransparency());
    auto keyed = assets.loadTexture("world/shared.blp");
    REQUIRE(keyed.data.size() == 64);
    keyed.data[3] = 0;
    CHECK(keyed.hasTransparency());
    CHECK_FALSE(source->image.hasTransparency());
    CHECK(source->image.data[3] == 255);
}
