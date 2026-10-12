/// Bringing a realm's patch directory in line with the list the realm sends.
///
/// The transport is faked: these tests decide what the "server" streams and
/// when the connection drops, and check what ends up on disk. The cases that
/// matter are the unhappy ones - a resumed file, a damaged one, one that runs
/// long, a connection lost halfway - because each of them must leave nothing
/// half-written where the client would read it.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <openssl/evp.h>

#include "core/realm_patch_sync.hpp"

namespace fs = std::filesystem;
using wowee::core::planRealmPatches;
using wowee::core::RealmPatchSync;
using wowee::game::EzWowPatch;

namespace {

std::array<uint8_t, 16> md5Of(const std::string& bytes) {
    std::array<uint8_t, 16> out{};
    unsigned size = 0;
    EVP_Digest(bytes.data(), bytes.size(), out.data(), &size, EVP_md5(), nullptr);
    return out;
}

EzWowPatch patch(const std::string& name, const std::string& content) {
    return {name, static_cast<uint32_t>(content.size()), 0, md5Of(content), {}};
}

std::string contentOf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write(const fs::path& path, const std::string& content) {
    std::ofstream(path, std::ios::binary) << content;
}

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("wowee-realm-patch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct FakeServer {
    std::vector<std::pair<std::string, uint64_t>> requests;
    int cancels = 0;
    bool up = true;
    RealmPatchSync::Transport transport() {
        return {[this](const std::string& name, uint64_t offset) {
                    if (!up) return false;
                    requests.emplace_back(name, offset);
                    return true;
                },
                [this] { ++cancels; }, [this] { return up; }};
    }
};

// Drive update() until the state leaves the given one, or give up.
RealmPatchSync::State settle(RealmPatchSync& sync) {
    for (int i = 0; i < 2000; ++i) {
        sync.update();
        const auto s = sync.progress().state;
        if (s != RealmPatchSync::State::Checking && s != RealmPatchSync::State::Verifying) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return sync.progress().state;
}

void send(RealmPatchSync& sync, const std::string& bytes, size_t chunk = 7) {
    for (size_t i = 0; i < bytes.size(); i += chunk) {
        const auto piece = bytes.substr(i, chunk);
        sync.onData(reinterpret_cast<const uint8_t*>(piece.data()), piece.size());
    }
}

} // namespace

TEST_CASE("the plan keeps this locale's archives and strips the load-order prefix", "[realm_patches]") {
    const std::vector<EzWowPatch> manifest = {
        patch("12_new_maps.mpq", "a"),      patch("02_initial_enGB.MPQ", "b"),
        patch("02_initial_ruRU.MPQ", "c"),  patch("01_dbc_diff.MPQ", "d"),
        patch("99_models_attach53.MPQ", "e"), patch("08_new_sounds_ruRU.MPQ", "f"),
    };
    const auto plan = planRealmPatches(manifest, "ruRU");
    REQUIRE(plan.size() == 5);
    CHECK(plan[0].localName == "dbc_diff.MPQ");
    CHECK(plan[0].order == 1);
    CHECK(plan[1].localName == "initial_ruRU.MPQ");
    CHECK(plan[2].localName == "new_sounds_ruRU.MPQ");
    CHECK(plan[3].localName == "new_maps.mpq");
    CHECK(plan[4].localName == "models_attach53.MPQ");
    // The server name is what the transfer asks for.
    CHECK(plan[1].patch.name == "02_initial_ruRU.MPQ");

    const auto english = planRealmPatches(manifest, "enGB");
    REQUIRE(english.size() == 4);
    CHECK(english[1].localName == "initial_enGB.MPQ");
}

TEST_CASE("a name without a prefix, or that is a path once stripped, is left out", "[realm_patches]") {
    const std::vector<EzWowPatch> manifest = {
        patch("new_maps.mpq", "a"), patch("05_..mpq", "b"), patch("05_a b.mpq", "c"),
        patch("05_lazy_cache.MPQ", "d"), patch("07_notes.txt", "e"),
    };
    const auto plan = planRealmPatches(manifest, "enUS");
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].localName == "cache.MPQ");
}

TEST_CASE("a missing archive is fetched, checked and moved into place", "[realm_patches]") {
    TempDir dir;
    FakeServer server;
    const std::string held = "already here";
    const std::string fresh = std::string(1000, 'x') + "fresh archive";
    write(dir.path / "held.MPQ", held);

    RealmPatchSync sync(dir.path, server.transport());
    sync.start({patch("01_held.MPQ", held), patch("02_fresh.MPQ", fresh)}, "enUS");
    REQUIRE(settle(sync) == RealmPatchSync::State::Downloading);
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests[0] == std::make_pair(std::string("02_fresh.MPQ"), uint64_t(0)));
    CHECK(sync.progress().totalBytes == fresh.size());

    send(sync, fresh);
    REQUIRE(settle(sync) == RealmPatchSync::State::Done);
    CHECK(contentOf(dir.path / "fresh.MPQ") == fresh);
    CHECK_FALSE(fs::exists(dir.path / "fresh.MPQ.partial"));
    CHECK(sync.archives() == std::vector<fs::path>{dir.path / "held.MPQ", dir.path / "fresh.MPQ"});

    // The next login finds everything current and asks for nothing.
    RealmPatchSync again(dir.path, server.transport());
    again.start({patch("01_held.MPQ", held), patch("02_fresh.MPQ", fresh)}, "enUS");
    CHECK(settle(again) == RealmPatchSync::State::Done);
    CHECK(server.requests.size() == 1);
}

TEST_CASE("an archive whose content changed is fetched again", "[realm_patches]") {
    TempDir dir;
    FakeServer server;
    write(dir.path / "hotfix.MPQ", "old version!");   // same length, other bytes
    RealmPatchSync sync(dir.path, server.transport());
    sync.start({patch("03_hotfix.MPQ", "new version!")}, "enUS");
    REQUIRE(settle(sync) == RealmPatchSync::State::Downloading);
    send(sync, "new version!");
    REQUIRE(settle(sync) == RealmPatchSync::State::Done);
    CHECK(contentOf(dir.path / "hotfix.MPQ") == "new version!");
}

TEST_CASE("an interrupted download resumes from what it holds", "[realm_patches]") {
    TempDir dir;
    FakeServer server;
    const std::string content = "0123456789abcdefghij";
    write(dir.path / "big.MPQ.partial", content.substr(0, 8));

    RealmPatchSync sync(dir.path, server.transport());
    sync.start({patch("05_big.MPQ", content)}, "enUS");
    REQUIRE(settle(sync) == RealmPatchSync::State::Downloading);
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests[0].second == 8);
    send(sync, content.substr(8));
    REQUIRE(settle(sync) == RealmPatchSync::State::Done);
    CHECK(contentOf(dir.path / "big.MPQ") == content);
}

TEST_CASE("a damaged download is discarded, never moved into place", "[realm_patches]") {
    TempDir dir;
    FakeServer server;
    RealmPatchSync sync(dir.path, server.transport());
    sync.start({patch("05_big.MPQ", "the real bytes")}, "enUS");
    REQUIRE(settle(sync) == RealmPatchSync::State::Downloading);
    send(sync, "the fake bytes");
    CHECK(settle(sync) == RealmPatchSync::State::Failed);
    CHECK_FALSE(fs::exists(dir.path / "big.MPQ"));
    CHECK_FALSE(fs::exists(dir.path / "big.MPQ.partial"));
}

TEST_CASE("more bytes than announced stops the transfer", "[realm_patches]") {
    TempDir dir;
    FakeServer server;
    RealmPatchSync sync(dir.path, server.transport());
    sync.start({patch("05_big.MPQ", "short")}, "enUS");
    REQUIRE(settle(sync) == RealmPatchSync::State::Downloading);
    send(sync, "short and then some", 64);
    CHECK(sync.progress().state == RealmPatchSync::State::Failed);
    CHECK(server.cancels == 1);
    CHECK_FALSE(fs::exists(dir.path / "big.MPQ"));
}

TEST_CASE("a dropped connection keeps the partial file for next time", "[realm_patches]") {
    TempDir dir;
    FakeServer server;
    RealmPatchSync sync(dir.path, server.transport());
    sync.start({patch("05_big.MPQ", "0123456789")}, "enUS");
    REQUIRE(settle(sync) == RealmPatchSync::State::Downloading);
    send(sync, "01234");
    server.up = false;
    sync.update();
    CHECK(sync.progress().state == RealmPatchSync::State::Failed);
    CHECK(contentOf(dir.path / "big.MPQ.partial") == "01234");
    CHECK_FALSE(fs::exists(dir.path / "big.MPQ"));
}
