#include <catch_amalgamated.hpp>
#include "ui/ui_texture_cache.hpp"
#include "ui/interface_art.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
using namespace wowee::ui;
using namespace std::chrono_literals;
namespace {
void until(const std::function<bool()>& done, UiTextureCache& cache) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        cache.pump(); std::this_thread::sleep_for(1ms);
    }
    REQUIRE(done());
}
auto descriptor(uintptr_t value) { return reinterpret_cast<VkDescriptorSet>(value); }
}
TEST_CASE("interface requests do not wait for disk and publish only fenced images", "[ui][resources]") {
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<int> reads{0};
    auto uploaded = std::make_shared<std::atomic<int>>(0);
    int published = 0;
    UiTextureCache cache([&](const std::string&, bool) {
        ++reads; gate.wait(); return PreparedUiTexture{{}, 64, 32, uploaded};
    }, [&](const auto&) { ++published; return descriptor(1); }, [](auto&) {});
    cache.request("Interface/Buttons/Close.tga");
    cache.request("interface\\buttons\\close.blp");
    cache.request("INTERFACE\\Buttons\\Close");
    cache.pump();
    CHECK(cache.resident("Interface\\Buttons\\Close") == nullptr);
    CHECK(published == 0);
    release.set_value();
    until([&] { return cache.state("Interface\\Buttons\\Close") == UiTextureCache::State::Uploading; }, cache);
    CHECK(reads == 1);
    CHECK(published == 0);
    uploaded->store(1);
    until([&] { return cache.pending() == 0; }, cache);
    REQUIRE(cache.resident("Interface/Buttons/Close"));
    CHECK(*cache.resident("Interface/Buttons/Close") == descriptor(1));
    float width = 0, height = 0;
    REQUIRE(cache.size("Interface/Buttons/Close", width, height));
    CHECK(width == 64); CHECK(height == 32);
    CHECK(published == 1);
}
TEST_CASE("ready interface art is published together without a three texture frame limit", "[ui][resources]") {
    auto uploaded = std::make_shared<std::atomic<int>>(0);
    UiTextureCache cache([&](const auto&, bool) { return PreparedUiTexture{{}, 16, 16, uploaded}; },
                         [](const auto&) { return descriptor(1); }, [](auto&) {});
    for (int i = 0; i < 8; ++i) cache.request("Interface/Test/" + std::to_string(i));
    until([&] {
        for (int i = 0; i < 8; ++i)
            if (cache.state("Interface/Test/" + std::to_string(i)) != UiTextureCache::State::Uploading) return false;
        return true;
    }, cache);
    uploaded->store(1);
    cache.pump();
    for (int i = 0; i < 8; ++i) {
        REQUIRE(cache.resident("Interface/Test/" + std::to_string(i)));
        CHECK(*cache.resident("Interface/Test/" + std::to_string(i)) == descriptor(1));
    }
}
TEST_CASE("bad interface art fails once and additive variants have their own image", "[ui][resources]") {
    std::atomic<int> failures{0}, normal{0}, additive{0};
    UiTextureCache cache([&](const std::string& path, bool add) {
        if (path.find("missing") != std::string::npos) { ++failures; throw std::runtime_error("missing BLP"); }
        if (add) ++additive; else ++normal;
        return PreparedUiTexture{{}, 16, 16, {}};
    }, [](const auto&) { return descriptor(1); }, [](auto&) {});
    cache.request("Interface/Test/missing");
    cache.request("Interface/Test/glow"); cache.request("Interface/Test/glow", true);
    until([&] { return cache.pending() == 0; }, cache);
    for (int i = 0; i < 10; ++i) {
        cache.request("Interface/Test/missing"); cache.request("Interface/Test/glow", true); cache.pump();
    }
    CHECK(failures == 1); CHECK(normal == 1); CHECK(additive == 1);
    REQUIRE(cache.resident("Interface/Test/missing"));
    CHECK(*cache.resident("Interface/Test/missing") == VK_NULL_HANDLE);
}
TEST_CASE("prefetch discovers hidden window art and leaves character icons for login", "[ui][resources]") {
    const std::vector<std::string> paths{
        "interface\\icons\\spell_fire.blp", "interface\\test\\panel-left.blp",
        "interface\\test\\panel-right.blp", "interface\\test\\unrelated.blp"};
    const std::string source = R"(
-- A player's hidden window; never run this script during character selection.
local prefix = "Interface\\Test\\Panel-"
local icons = "Interface\\Icons\\"
local icon = "Interface\\Icons\\Spell_Fire"
<Ui><Frame hidden="true"><Layers><Layer><Texture file="Interface\Test\Glow" alphaMode="ADD"/></Layer></Layers></Frame></Ui>
)";
    const auto art = interfaceArtReferences(source, paths);
    const auto has = [&](const char* path, bool add = false) {
        return std::any_of(art.begin(), art.end(), [&](const auto& ref) { return ref.path == path && ref.additive == add; });
    };
    CHECK(has("interface\\test\\panel-left.blp"));
    CHECK(has("interface\\test\\panel-right.blp"));
    CHECK_FALSE(has("interface\\icons\\spell_fire.blp"));
    CHECK_FALSE(has("interface\\test\\unrelated.blp"));
}
TEST_CASE("hidden additive art is prefetched without enumerating the icon picker", "[ui][resources]") {
    const std::vector<std::string> paths{"interface\\buttons\\glow.blp", "interface\\icons\\spell_fire.blp"};
    const auto art = interfaceArtReferences(R"(<Ui><Texture hidden="true" file="Interface\Buttons\Glow.tga" alphaMode="ADD"/><Texture file="Interface\Icons\Spell_Fire"/></Ui>)", paths);
    REQUIRE(art.size() == 2);
    CHECK(art[0].path == "interface\\buttons\\glow.blp");
    CHECK(art[1].path == "interface\\buttons\\glow.blp");
    CHECK_FALSE(art[0].additive);
    CHECK(art[1].additive);
}
TEST_CASE("a failed GPU copy never becomes drawable", "[ui][resources]") {
    auto uploaded = std::make_shared<std::atomic<int>>(0);
    int published = 0;
    UiTextureCache cache([&](const auto&, bool) { return PreparedUiTexture{{}, 16, 16, uploaded}; },
        [&](const auto&) { ++published; return descriptor(1); }, [](auto&) {});
    cache.request("Interface/Test/fault");
    until([&] { return cache.state("Interface/Test/fault") == UiTextureCache::State::Uploading; }, cache);
    uploaded->store(-1);
    cache.pump();
    CHECK(published == 0);
    REQUIRE(cache.resident("Interface/Test/fault"));
    CHECK(*cache.resident("Interface/Test/fault") == VK_NULL_HANDLE);
    cache.request("Interface/Test/fault"); cache.pump();
    CHECK(cache.pending() == 0);
}
TEST_CASE("a visible icon joins ahead of unopened window art", "[ui][resources]") {
    std::mutex mutex;
    std::vector<std::string> loaded;
    UiTextureCache cache([&](const std::string& path, bool) {
        std::lock_guard lock(mutex); loaded.push_back(path);
        return PreparedUiTexture{{}, 16, 16, {}};
    }, [](const auto&) { return descriptor(1); }, [](auto&) {});
    for (int i = 0; i < 20; ++i) cache.request("Interface/Window/" + std::to_string(i));
    cache.request("Interface/Icon/visible", false, true);
    cache.pump();
    until([&] { return cache.resident("Interface/Icon/visible") != nullptr; }, cache);
    REQUIRE(cache.resident("Interface/Icon/visible"));
    CHECK(*cache.resident("Interface/Icon/visible") == descriptor(1));
    // Draining the rest never reloads that published icon.
    until([&] { return cache.pending() == 0; }, cache);
    CHECK(*cache.resident("Interface/Icon/visible") == descriptor(1));
    CHECK(std::find(loaded.begin(), loaded.end(), "interface\\icon\\visible.blp") <
          std::find(loaded.begin(), loaded.end(), "interface\\window\\19.blp"));
}
