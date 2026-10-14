// The CVar store the game, the interface and the addons share.
#include <catch_amalgamated.hpp>

#include "core/cvar_store.hpp"
#include "test_support.hpp"

#include <fstream>
#include <string>

using wowee::core::CVarStore;

TEST_CASE("an unset CVar answers its fallback", "[cvars]") {
    CVarStore store;
    store.load();
    CHECK(store.get("autoClearAFK", "1") == "1");
    CHECK(store.find("autoClearAFK") == nullptr);
}

TEST_CASE("a name is one CVar whatever its case", "[cvars]") {
    CVarStore store;
    store.load();
    CHECK(store.set("uiScale", "0.9"));
    CHECK(store.get("uiscale", "1") == "0.9");
    CHECK(store.get("UISCALE", "1") == "0.9");
    // The same value again is not a change, so nothing is written for it.
    CHECK_FALSE(store.set("UiScale", "0.9"));
}

TEST_CASE("a value survives the client closing", "[cvars]") {
    const auto dir = wowee::test::scratchDir("wowee_cvar_store");
    const std::string path = (dir / "cvars.cfg").string();
    {
        CVarStore store(path);
        store.load();
        store.set("assistAttack", "1");
        // Written under a mixed-case name and read back under it after a
        // restart: the tutorial flags were stored as "wowee_tutorialsFlagged",
        // reloaded as "wowee_tutorialsflagged" and then looked up under the
        // first spelling, so every tutorial came back on every start.
        store.set("wowee_tutorialsFlagged", "1,4,9");
    }
    CVarStore reopened(path);
    reopened.load();
    CHECK(reopened.get("assistAttack", "0") == "1");
    CHECK(reopened.get("wowee_tutorialsFlagged", "") == "1,4,9");
}

TEST_CASE("the file is read once, before anything sets a value", "[cvars]") {
    const auto dir = wowee::test::scratchDir("wowee_cvar_store_once");
    const std::string path = (dir / "cvars.cfg").string();
    {
        std::ofstream out(path);
        out << "spamfilter=0\n";
    }
    CVarStore store(path);
    store.load();
    store.set("spamFilter", "1");
    // A second load must not put the file's older value back.
    store.load();
    CHECK(store.get("spamFilter", "") == "1");
}
