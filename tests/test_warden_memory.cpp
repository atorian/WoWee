#include <catch_amalgamated.hpp>
#include "game/warden_memory.hpp"
#include "core/env.hpp"
#include "core/logger.hpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct SavedEnvironment {
    const char* name;
    std::optional<std::string> value;
    explicit SavedEnvironment(const char* n) : name(n) {
        if (const auto* v = std::getenv(n)) value = v;
        wowee::core::unsetEnvVar(n);
    }
    ~SavedEnvironment() {
        if (value) wowee::core::setEnvVar(name, value->c_str());
        else wowee::core::unsetEnvVar(name);
    }
};
struct Fixture {
    fs::path root = fs::temp_directory_path() /
        ("wowee_warden_memory_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    SavedEnvironment integrity{"WOWEE_INTEGRITY_DIR"};
    SavedEnvironment data{"WOW_DATA_PATH"};
    Fixture() {
        fs::create_directories(root);
        wowee::core::setEnvVar("WOW_DATA_PATH", root.string().c_str());
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    void exe(const fs::path& directory, uint8_t marker, size_t extraBytes = 0) {
        fs::create_directories(directory);
        std::vector<uint8_t> bytes(0x500 + extraBytes, 0);
        auto w32 = [&](size_t at, uint32_t n) {
            for (int i = 0; i < 4; ++i) bytes[at + i] = n >> (8 * i);
        };
        auto w16 = [&](size_t at, uint16_t n) { bytes[at] = n; bytes[at + 1] = n >> 8; };
        bytes[0] = 'M'; bytes[1] = 'Z'; w32(0x3C, 0x80);
        bytes[0x80] = 'P'; bytes[0x81] = 'E';
        w16(0x84 + 2, 1); w16(0x84 + 16, 0xE0);
        constexpr size_t optional = 0x98;
        w16(optional, 0x10B); w32(optional + 28, 0x400000);
        w32(optional + 56, 0x2000); w32(optional + 60, 0x400);
        constexpr size_t section = optional + 0xE0;
        w32(section + 8, 0x100); w32(section + 12, 0x1000);
        w32(section + 16, 0x100); w32(section + 20, 0x400);
        bytes[0x2A7] = marker; // Same header address used by the realm's login gate.
        std::ofstream out(directory / "WoW.exe", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
};
}

TEST_CASE("Warden reads the selected WotLK extraction instead of a larger Classic cache", "[warden]") {
    Fixture f;
    const auto active = f.root / "expansions/wotlk";
    f.exe(active / "misc", 0xE0);
    f.exe(f.root / "expansions/classic/misc", 0x11, 1024);
    f.exe(f.root / "misc", 0x22, 2048);
    wowee::game::WardenMemory memory;
    REQUIRE(memory.load(12340, false, active.string()));
    uint8_t result = 0;
    REQUIRE(memory.readMemory(0x4002A7, 1, &result));
    CHECK(result == 0xE0);
}

TEST_CASE("An explicit Warden integrity directory wins over the active extraction", "[warden]") {
    Fixture f;
    const auto active = f.root / "expansions/wotlk";
    const auto overrideDir = f.root / "override";
    f.exe(active / "misc", 0xE0, 1024);
    f.exe(overrideDir, 0x33);
    wowee::core::setEnvVar("WOWEE_INTEGRITY_DIR", overrideDir.string().c_str());
    wowee::game::WardenMemory memory;
    REQUIRE(memory.load(12340, false, active.string()));
    uint8_t result = 0;
    REQUIRE(memory.readMemory(0x4002A7, 1, &result));
    CHECK(result == 0x33);
}
