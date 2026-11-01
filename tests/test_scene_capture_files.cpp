// What a scene replay accepts of the files its capture was taken against: the
// same manifest and realm archives, however long ago and on whichever side of
// a daylight-saving change, and the same data root however it is spelled -
// and nothing whose content changed.
#include <catch_amalgamated.hpp>

#include "core/scene_capture_files.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#endif

using wowee::core::signCapturedFile;
using wowee::core::verifyCapturedDataPath;
using wowee::core::verifyCapturedFile;

namespace {

namespace fs = std::filesystem;

// Unix nanoseconds of a UTC calendar instant.
int64_t utcNs(int year, unsigned month, unsigned day, int hour) {
    using namespace std::chrono;
    const auto when = sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}} + hours{hour};
    return duration_cast<nanoseconds>(when.time_since_epoch()).count();
}

// Sets a file's last write, given in UTC, through the operating system and
// not through std::filesystem, whose Windows clock is the one under test.
void setWritten(const fs::path& path, int64_t unixNs) {
#ifdef _WIN32
    const auto ticks = static_cast<uint64_t>(unixNs / 100 + 116444736000000000);
    FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    const HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    const bool set = SetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
    REQUIRE(set);
#else
    const timespec times[2] = {{0, UTIME_OMIT}, {static_cast<time_t>(unixNs / 1000000000), static_cast<long>(unixNs % 1000000000)}};
    REQUIRE(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
#endif
}

void write(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    REQUIRE(out.good());
}

struct Scratch {
    fs::path root = fs::temp_directory_path() / "wowee_scene_capture_files";
    Scratch() { fs::remove_all(root); fs::create_directories(root / "Data" / "expansions" / "wotlk"); }
    ~Scratch() { std::error_code ignored; fs::remove_all(root, ignored); }
};

std::string refusal(const auto& call) {
    try { call(); } catch (const std::runtime_error& e) { return e.what(); }
    return {};
}

}

TEST_CASE("A capture's file verifies while it is unchanged", "[scene_replay]") {
    Scratch scratch;
    const auto archive = scratch.root / "dbc_diff.MPQ";
    write(archive, "realm patch");
    setWritten(archive, utcNs(2026, 7, 15, 12));
    const auto signature = signCapturedFile(archive);
    CHECK_NOTHROW(verifyCapturedFile(signature));
}

TEST_CASE("A capture records a file's write in UTC on either side of daylight saving", "[scene_replay]") {
    // Summer and winter instants: a local-time clock is an hour apart from
    // UTC in one of them on any machine whose zone keeps daylight saving.
    Scratch scratch;
    const auto file = scratch.root / "manifest.json";
    write(file, "{}");
    for (const auto instant : {utcNs(2026, 7, 15, 12), utcNs(2026, 1, 15, 12)}) {
        setWritten(file, instant);
        CHECK(signCapturedFile(file).at("written").get<int64_t>() == instant);
    }
}

TEST_CASE("A capture's file rewritten in place is refused by name", "[scene_replay]") {
    Scratch scratch;
    const auto archive = scratch.root / "new_icons.MPQ";
    write(archive, "realm patch");
    setWritten(archive, utcNs(2026, 10, 22, 18));
    const auto signature = signCapturedFile(archive);

    SECTION("same size, new content") {
        write(archive, "REALM PATCH");
        setWritten(archive, utcNs(2026, 10, 31, 9));
    }
    SECTION("another size, old write time") {
        write(archive, "realm patch, replaced");
        setWritten(archive, utcNs(2026, 10, 22, 18));
    }
    const auto why = refusal([&] { verifyCapturedFile(signature); });
    CHECK(why.find("capture asset changed") != std::string::npos);
    CHECK(why.find(archive.string()) != std::string::npos);
}

TEST_CASE("A signature in the old local-time form is refused with the way out", "[scene_replay]") {
    Scratch scratch;
    const auto manifest = scratch.root / "manifest.json";
    write(manifest, "{}");
    auto signature = signCapturedFile(manifest);
    signature.erase("written");
    signature["modified"] = -4644973466000000000;
    const auto why = refusal([&] { verifyCapturedFile(signature); });
    CHECK(why.find(manifest.string()) != std::string::npos);
    CHECK(why.find("tools/resign_scene_capture.py") != std::string::npos);
}

TEST_CASE("The captured data root is the same directory however it is spelled", "[scene_replay]") {
    Scratch scratch;
    const auto data = scratch.root / "Data";
    const auto expansion = data / "expansions" / "wotlk";
    const auto generic = expansion.generic_string();
    // The form captures before 2026-11-01 recorded: the expansion joined to
    // the root with a forward slash and the rest native.
    const auto mixed = data.string() + "/expansions" + static_cast<char>(fs::path::preferred_separator) + "wotlk";
    CHECK_NOTHROW(verifyCapturedDataPath(generic, fs::path(expansion).make_preferred().string()));
    CHECK_NOTHROW(verifyCapturedDataPath(generic, mixed));
    CHECK_NOTHROW(verifyCapturedDataPath(generic, (data / "expansions" / "." / "wotlk").string()));

    fs::create_directories(data / "expansions" / "tbc");
    const auto other = refusal([&] { verifyCapturedDataPath((data / "expansions" / "tbc").generic_string(), mixed); });
    CHECK(other.find("is not the captured") != std::string::npos);
    const auto missing = refusal([&] { verifyCapturedDataPath(generic, (data / "expansions" / "classic").string()); });
    CHECK(missing.find("cannot compare") != std::string::npos);
}
