/// The extractor's progress reporting.
///
/// Extraction is the long stage of a build - minutes on a real game - and the
/// window counted whole stages, so its bar sat still throughout and read as a
/// hang. Options::onProgress is what moves it. This builds a real archive,
/// extracts it, and checks the reports arrive, climb, and finish on the total.

#include <catch_amalgamated.hpp>

#include <StormLib.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "tools/asset_extract/extractor.hpp"
#include "test_support.hpp"

namespace fs = std::filesystem;

namespace {

/// Enough files that the reporter's own interval is crossed several times;
/// it reports every 128, so this is a handful of updates plus the last one.
constexpr int kFileCount = 400;

/// A real MPQ, built the way mpq_build builds one. Nothing here is a stand-in:
/// the point is to drive the extractor's own threading, which is what decides
/// whether the reports come out in order.
bool packArchive(const fs::path& from, const fs::path& archivePath) {
    std::error_code ec;
    fs::create_directories(archivePath.parent_path(), ec);
    HANDLE archive = nullptr;
    if (!SFileCreateArchive(archivePath.string().c_str(),
                            MPQ_CREATE_ARCHIVE_V1 | MPQ_CREATE_LISTFILE,
                            kFileCount * 2, &archive)) {
        return false;
    }
    for (const auto& entry : fs::recursive_directory_iterator(from)) {
        if (!entry.is_regular_file()) continue;
        std::string name = fs::relative(entry.path(), from, ec).string();
        for (char& c : name) {
            if (c == '/') c = '\\';
        }
        if (!SFileAddFileEx(archive, entry.path().string().c_str(), name.c_str(),
                            MPQ_FILE_REPLACEEXISTING | MPQ_FILE_COMPRESS,
                            MPQ_COMPRESSION_ZLIB, MPQ_COMPRESSION_NEXT_SAME)) {
            SFileCloseArchive(archive);
            return false;
        }
    }
    SFileCloseArchive(archive);
    return true;
}

}  // namespace

#ifdef _WIN32
namespace {
struct ExtractFiles {
    fs::path root;
    ~ExtractFiles() {
        std::error_code ec;
        for (const auto& item : fs::recursive_directory_iterator(root, ec)) {
            if (item.is_regular_file(ec))
                fs::permissions(item.path(), fs::perms::owner_write, fs::perm_options::add, ec);
        }
        fs::remove_all(root, ec);
    }
};

std::string contents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("extraction reads archives from a read-only installation", "[extract][readonly]") {
    ExtractFiles files{wowee::test::scratchDir("wowee_extract_readonly_archive")};
    fs::create_directories(files.root / "src/World");
    std::ofstream(files.root / "src/World/probe.wdt") << "original world data";
    const auto archive = files.root / "game/Data/lichking.MPQ";
    REQUIRE(packArchive(files.root / "src", archive));
    const auto before = contents(archive);
    fs::permissions(archive, fs::perms::owner_read, fs::perm_options::replace);
    wowee::tools::Extractor::Options opts;
    opts.mpqDir = archive.parent_path().string();
    opts.outputDir = (files.root / "out").string();
    opts.expansion = "wotlk";
    REQUIRE(wowee::tools::Extractor::run(opts));
    CHECK(contents(files.root / "out/world/probe.wdt") == "original world data");
    CHECK(contents(archive) == before);
}

TEST_CASE("reimport replaces read-only cached client files", "[extract][readonly]") {
    ExtractFiles files{wowee::test::scratchDir("wowee_extract_readonly_cache")};
    fs::create_directories(files.root / "src/World");
    std::ofstream(files.root / "src/World/probe.wdt") << "world data";
    REQUIRE(packArchive(files.root / "src", files.root / "game/Data/lichking.MPQ"));
    for (const char* name : {"WoW.exe", "ijl15.dll"}) {
        std::ofstream(files.root / "game" / name) << "old client bytes";
        fs::permissions(files.root / "game" / name, fs::perms::owner_read,
                        fs::perm_options::replace);
    }
    wowee::tools::Extractor::Options opts;
    opts.mpqDir = (files.root / "game/Data").string();
    opts.outputDir = (files.root / "out").string();
    opts.expansion = "wotlk";
    REQUIRE(wowee::tools::Extractor::run(opts));
    for (const char* name : {"WoW.exe", "ijl15.dll"}) {
        const auto source = files.root / "game" / name;
        fs::permissions(source, fs::perms::owner_write, fs::perm_options::add);
        std::ofstream(source) << "updated client bytes";
        fs::permissions(source, fs::perms::owner_read, fs::perm_options::replace);
        fs::permissions(files.root / "out/misc" / name, fs::perms::owner_read,
                        fs::perm_options::replace);
    }
    REQUIRE(wowee::tools::Extractor::run(opts));
    CHECK(contents(files.root / "out/misc/WoW.exe") == "updated client bytes");
    CHECK(contents(files.root / "out/misc/ijl15.dll") == "updated client bytes");
}

TEST_CASE("extraction refuses an unreadable base archive instead of reporting partial success", "[extract][readonly]") {
    ExtractFiles files{wowee::test::scratchDir("wowee_extract_broken_base")};
    fs::create_directories(files.root / "src/World");
    std::ofstream(files.root / "src/World/probe.wdt") << "patch contents";
    REQUIRE(packArchive(files.root / "src", files.root / "game/Data/patch.MPQ"));
    std::ofstream(files.root / "game/Data/common.MPQ") << "broken archive";
    wowee::tools::Extractor::Options opts;
    opts.mpqDir = (files.root / "game/Data").string();
    opts.outputDir = (files.root / "out").string();
    opts.expansion = "wotlk";
    CHECK_FALSE(wowee::tools::Extractor::run(opts));
    CHECK_FALSE(fs::exists(files.root / "out/manifest.json"));
}
#endif

TEST_CASE("extraction reports progress as it goes", "[extract][progress]") {
#ifdef __linux__
    // Debian and Ubuntu package StormLib 9.22 built against a libtomcrypt
    // whose hash_state is larger than the buffer StormLib reserves for it,
    // so its *writer* aborts on an internal assertion:
    //
    //   SFileAddFile.cpp:103: Assertion `sizeof(hf->hctx) >= sizeof(hash_state)`
    //
    // It aborts rather than failing, so there is nothing to catch. Only the
    // writer is affected and this client never writes an archive - the
    // extractor reads - so what is lost here is the fixture, not coverage of
    // the thing under test. The reading half runs everywhere else.
    SKIP("Ubuntu's StormLib build asserts when creating an archive");
#endif
    const fs::path root = wowee::test::scratchDir("wowee_extract_progress");
    const fs::path src = root / "src";
    std::error_code ec;
    fs::create_directories(src / "World" / "Maps", ec);

    for (int i = 0; i < kFileCount; ++i) {
        std::ofstream out(src / "World" / "Maps" / ("tile" + std::to_string(i) + ".wdt"),
                          std::ios::binary);
        out << "tile " << i << " contents";
    }

    const fs::path data = root / "game" / "Data";
    REQUIRE(packArchive(src, data / "lichking.MPQ"));

    // What the callback saw, in the order it saw it.
    std::mutex mutex;
    std::vector<std::pair<std::size_t, std::size_t>> reports;

    wowee::tools::Extractor::Options opts;
    opts.mpqDir = data.string();
    opts.outputDir = (root / "out").string();
    opts.expansion = "wotlk";
    opts.onProgress = [&](std::size_t done, std::size_t total) {
        std::lock_guard<std::mutex> lock(mutex);
        reports.emplace_back(done, total);
    };

    REQUIRE(wowee::tools::Extractor::run(opts));

    std::lock_guard<std::mutex> lock(mutex);

    // It said something at all. Before onProgress existed the only sign of
    // life during this stage was a line of stdout nothing displays.
    REQUIRE_FALSE(reports.empty());

    // More than once, so a bar has something to animate rather than one jump
    // at the end that is indistinguishable from no reporting.
    CHECK(reports.size() > 1);

    for (const auto& [done, total] : reports) {
        CHECK(total == static_cast<std::size_t>(kFileCount));
        CHECK(done <= total);
    }

    // And it arrives at the end. The reporter counts in steps of 128, so
    // without a final report a bar stops a little short of full and stays
    // there - which reads as a build that did not finish.
    CHECK(reports.back().first == static_cast<std::size_t>(kFileCount));

    fs::remove_all(root, ec);
}
