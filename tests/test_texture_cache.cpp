// The renderers' one texture cache: one entry per key whoever asks, one
// budget, eviction of only what nothing references, least recently used
// first, and nothing freed on the GPU before the frames that may read it are
// done. The device is a fake whose frame fences the test passes by hand, and
// a texture is a token whose destruction the test can see.
#include <catch_amalgamated.hpp>

#include "rendering/texture_cache.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using wowee::rendering::normalizeTexturePath;
using wowee::rendering::TextureCache;
using wowee::rendering::TextureDevice;
using wowee::rendering::TextureForm;
using wowee::rendering::TextureKey;
using wowee::rendering::TextureRef;
using wowee::rendering::VkTexture;

namespace {

constexpr uint64_t kMiB = 1024ull * 1024;

/// Frames in flight that have not fenced yet, and the driver's heap budget.
class FakeDevice : public TextureDevice {
public:
    void retireAfterFrames(std::function<void()> release) override { pending_.push_back(std::move(release)); }
    [[nodiscard]] std::optional<uint64_t> textureHeapBudget() const override { return heapBudget; }

    /// Every frame that was in flight has fenced.
    void fencesPassed() {
        auto pending = std::move(pending_);
        pending_.clear();
        for (auto& release : pending) release();
    }

    std::optional<uint64_t> heapBudget;

private:
    std::vector<std::function<void()>> pending_;
};

/// Stands in for an uploaded image: a distinct pointer nothing reads, whose
/// release on the GPU is recorded by name.
class Images {
public:
    std::shared_ptr<VkTexture> make(const std::string& name) {
        auto* token = reinterpret_cast<VkTexture*>(new std::byte);
        return {token, [this, name](VkTexture* t) {
                    destroyed.insert(name);
                    delete reinterpret_cast<std::byte*>(t);
                }};
    }
    std::set<std::string> destroyed;
};

TextureKey stored(const std::string& path) { return {normalizeTexturePath(path), TextureForm::Stored, 0}; }

TextureCache::Config budget(uint64_t bytes) {
    TextureCache::Config config;
    config.budgetBytes = bytes;
    return config;
}

} // namespace

TEST_CASE("one path asked for by two renderers is one texture", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(64 * kMiB));

    // The M2 renderer loads it under its own spelling...
    TextureRef forDoodad = cache.find(stored("World/Generic/Stone.BLP"));
    REQUIRE_FALSE(forDoodad);
    forDoodad = cache.insert(stored("World/Generic/Stone.BLP"), images.make("stone"), 4 * kMiB);
    // ...and the WMO renderer finds it under its own.
    TextureRef forWall = cache.find(stored(".\\WORLD\\generic\\stone.blp"));

    REQUIRE(forWall);
    CHECK(forWall.get() == forDoodad.get());
    CHECK(cache.stats().entries == 1);
    CHECK(cache.stats().bytes == 4 * kMiB);
}

TEST_CASE("a normal map is an entry of its own under the same path, and counted", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(64 * kMiB));

    TextureRef diffuse = cache.insert(stored("wall.blp"), images.make("diffuse"), 1 * kMiB);
    TextureRef normals = cache.insert({"wall.blp", TextureForm::NormalHeight, 2}, images.make("normals"),
                                      3 * kMiB);

    CHECK(diffuse.get() != normals.get());
    const auto stats = cache.stats();
    CHECK(stats.entries == 2);
    CHECK(stats.bytes == 4 * kMiB);
    CHECK(stats.form(TextureForm::NormalHeight).bytes == 3 * kMiB);
    // Another strength is another image.
    CHECK_FALSE(cache.find({"wall.blp", TextureForm::NormalHeight, 5}));
}

TEST_CASE("the cache never holds more than its budget", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(10 * kMiB));

    for (int i = 0; i < 20; ++i) {
        // Each is used and let go, as a model streaming past would.
        TextureRef ref = cache.insert(stored("tex" + std::to_string(i)), images.make("t"), 3 * kMiB);
        REQUIRE(ref);
        CHECK(cache.stats().bytes <= 10 * kMiB);
    }
    CHECK(cache.stats().bytes == 9 * kMiB);
}

TEST_CASE("the budget is no more than a share of what the driver allows", "[texture_cache]") {
    FakeDevice device;
    Images images;
    device.heapBudget = 8 * kMiB;
    TextureCache::Config config = budget(64 * kMiB);
    config.maxShareOfHeapBudget = 0.5;
    TextureCache cache(device, config);

    CHECK(cache.budgetBytes() == 4 * kMiB);
    TextureRef first = cache.insert(stored("a"), images.make("a"), 3 * kMiB);
    CHECK(first);
    // Held, so nothing can make room for a second.
    CHECK_FALSE(cache.insert(stored("b"), images.make("b"), 3 * kMiB));
}

TEST_CASE("only textures nothing references are evicted, oldest use first", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(12 * kMiB));

    TextureRef held = cache.insert(stored("held"), images.make("held"), 4 * kMiB);
    { TextureRef r = cache.insert(stored("old"), images.make("old"), 4 * kMiB); }
    { TextureRef r = cache.insert(stored("recent"), images.make("recent"), 4 * kMiB); }
    // "old" is looked at again, so "recent" is now the one used longest ago.
    { TextureRef r = cache.find(stored("old")); }

    TextureRef incoming = cache.insert(stored("incoming"), images.make("incoming"), 4 * kMiB);
    REQUIRE(incoming);
    CHECK(cache.find(stored("held")));
    CHECK(cache.find(stored("old")));
    CHECK_FALSE(cache.find(stored("recent")));
}

TEST_CASE("with everything in use a new texture is refused, and fits once something is released",
          "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(8 * kMiB));

    TextureRef a = cache.insert(stored("a"), images.make("a"), 4 * kMiB);
    TextureRef b = cache.insert(stored("b"), images.make("b"), 4 * kMiB);
    CHECK_FALSE(cache.insert(stored("c"), images.make("c"), 4 * kMiB));
    CHECK(cache.stats().refusals == 1);
    // Nothing that was in use went.
    CHECK(a.get() != nullptr);
    CHECK(cache.find(stored("b")));

    a.reset();
    CHECK(cache.insert(stored("c"), images.make("c"), 4 * kMiB));
    CHECK_FALSE(cache.find(stored("a")));
}

TEST_CASE("an evicted texture is freed only after the frames in flight have fenced", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(4 * kMiB));

    { TextureRef r = cache.insert(stored("first"), images.make("first"), 4 * kMiB); }
    TextureRef second = cache.insert(stored("second"), images.make("second"), 4 * kMiB);
    REQUIRE(second);

    // Out of the cache, not off the GPU: a command buffer may still sample it.
    CHECK_FALSE(cache.find(stored("first")));
    CHECK(images.destroyed.empty());
    CHECK(cache.stats().retiringBytes == 4 * kMiB);

    device.fencesPassed();
    CHECK(images.destroyed == std::set<std::string>{"first"});
    CHECK(cache.stats().retiringBytes == 0);
}

TEST_CASE("a texture only its owner can find goes with the owner's last reference", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(64 * kMiB));

    TextureRef alpha = cache.adopt(TextureForm::Owned, images.make("alpha"), 1 * kMiB);
    TextureRef copy = alpha;
    alpha.reset();
    CHECK(cache.stats().form(TextureForm::Owned).entries == 1);
    copy.reset();
    CHECK(cache.stats().entries == 0);
    CHECK(images.destroyed.empty());
    device.fencesPassed();
    CHECK(images.destroyed == std::set<std::string>{"alpha"});
}

TEST_CASE("a texture's normal map stays as long as the texture and goes with it", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(8 * kMiB));

    TextureRef skin = cache.insert(stored("skin.blp"), images.make("skin"), 2 * kMiB);
    cache.setNormalMap(skin, cache.insert({"skin.blp", TextureForm::NormalHeight, 5}, images.make("skin normals"),
                                          2 * kMiB, {.hasAlpha = false, .alphaIsSilhouette = false,
                                                     .heightVariance = 0.25f}));
    REQUIRE(skin.normalMap() != nullptr);
    CHECK(skin.normalMapInfo().heightVariance == 0.25f);

    // Nothing references the normal map but the skin: room is not made from it.
    TextureRef other = cache.insert(stored("other"), images.make("other"), 4 * kMiB);
    REQUIRE(other);
    CHECK(cache.find({"skin.blp", TextureForm::NormalHeight, 5}));

    // The skin let go and evicted: its normal map is let go with it, and goes
    // when room is wanted.
    skin.reset();
    other.reset();
    TextureRef big = cache.insert(stored("big"), images.make("big"), 8 * kMiB);
    REQUIRE(big);
    device.fencesPassed();
    CHECK(images.destroyed == std::set<std::string>{"skin", "skin normals", "other"});
}

TEST_CASE("a second upload of a resident key is retired and the first shared", "[texture_cache]") {
    FakeDevice device;
    Images images;
    TextureCache cache(device, budget(64 * kMiB));

    TextureRef first = cache.insert(stored("dup"), images.make("first"), 2 * kMiB);
    TextureRef second = cache.insert(stored("dup"), images.make("second"), 2 * kMiB);
    CHECK(first.get() == second.get());
    CHECK(cache.stats().bytes == 2 * kMiB);
    device.fencesPassed();
    CHECK(images.destroyed == std::set<std::string>{"second"});
}
