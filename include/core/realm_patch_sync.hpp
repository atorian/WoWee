#pragma once

/// Keeps a realm's patch archives on disk in step with the list the realm
/// announces.
///
/// EZWoW (Isengard) sends that list over the world connection after its
/// loader request; the files themselves stream over the still-open login
/// connection as ordinary XFER data, one named file at a time. This class
/// owns the bookkeeping between the two: which archives this client needs,
/// which it already holds intact, where an interrupted download resumes, and
/// whether what arrived is what was announced. The transport is a pair of
/// callbacks so the logic stays testable without a server.
///
/// Every name is checked before it touches the filesystem, every archive is
/// written beside its final name and moved into place only after its MD5
/// matches, and the directory comes from the caller (derived from the address
/// connected to), never from the server.

#include "game/server_protocol.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wowee::core {

/// One archive this client should hold, with the name it is kept under.
struct RealmPatchPlanEntry {
    game::EzWowPatch patch;
    /// The server name less its load-order prefix: "02_initial_ruRU.MPQ" is
    /// kept as "initial_ruRU.MPQ", as the original client does.
    std::string localName;
    /// The load-order number from the prefix; archives mount in this order.
    int order = 0;
};

/// The archives of `manifest` meant for `locale`, in load order.
///
/// A name carrying a locale suffix ("_ruRU") is kept only for that locale;
/// one without a suffix is kept for every locale. Names that do not have the
/// "<number>_" prefix, or that would not be a plain file name once it is
/// removed, are refused - the list says nothing about them that can be
/// trusted.
[[nodiscard]] std::vector<RealmPatchPlanEntry> planRealmPatches(
    const std::vector<game::EzWowPatch>& manifest, const std::string& locale);

class RealmPatchSync {
public:
    enum class State { Idle, Checking, Downloading, Verifying, Done, Failed };

    struct Transport {
        /// Ask for `serverName` from `offset`; false when that is impossible.
        std::function<bool(const std::string& serverName, uint64_t offset)> request;
        std::function<void()> cancel;
        std::function<bool()> connected;
    };

    struct Progress {
        State state = State::Idle;
        std::string current;      // local name of the archive in flight
        size_t fileIndex = 0;     // 1-based among the archives to download
        size_t fileCount = 0;
        uint64_t doneBytes = 0;   // across the archives to download
        uint64_t totalBytes = 0;
        std::string error;
    };

    RealmPatchSync(std::filesystem::path directory, Transport transport);
    ~RealmPatchSync();
    RealmPatchSync(const RealmPatchSync&) = delete;
    RealmPatchSync& operator=(const RealmPatchSync&) = delete;

    /// Begin bringing the directory in line with `manifest`. Checking runs on
    /// a worker; call update() each frame to move things along.
    void start(const std::vector<game::EzWowPatch>& manifest, const std::string& locale);
    void update();
    /// Bytes of the file in flight, in order.
    void onData(const uint8_t* data, size_t size);

    [[nodiscard]] Progress progress() const;
    [[nodiscard]] bool busy() const {
        return state_ == State::Checking || state_ == State::Downloading || state_ == State::Verifying;
    }
    /// Every planned archive, in load order, with its path on disk. Only
    /// meaningful once the state is Done.
    [[nodiscard]] std::vector<std::filesystem::path> archives() const;
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

private:
    struct CachedSum {
        uint64_t size = 0;
        int64_t mtime = 0;
        std::array<uint8_t, 16> md5{};
    };
    struct CheckResult {
        std::vector<size_t> missing;      // indices into plan_
        std::map<std::string, CachedSum> sums;
    };

    void beginNext();
    void fail(const std::string& reason);
    void closeFile();
    void loadCache();
    void saveCache() const;

    std::filesystem::path directory_;
    Transport transport_;
    State state_ = State::Idle;
    std::string error_;

    std::vector<RealmPatchPlanEntry> plan_;
    std::vector<size_t> queue_;            // plan_ indices still to download
    size_t queuePos_ = 0;
    std::map<std::string, CachedSum> sums_; // keyed by local name

    std::future<CheckResult> checking_;
    std::future<std::optional<std::array<uint8_t, 16>>> verifying_;

    std::FILE* file_ = nullptr;
    uint64_t received_ = 0;                 // bytes of the current archive held
    uint64_t completedBytes_ = 0;           // archives finished this run
    uint64_t totalBytes_ = 0;
};

/// MD5 of a whole file, or nothing when it cannot be read.
[[nodiscard]] std::optional<std::array<uint8_t, 16>> md5OfFile(const std::filesystem::path& path);

} // namespace wowee::core
