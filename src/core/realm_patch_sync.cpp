#include "core/realm_patch_sync.hpp"
#include "core/realm_patches.hpp"
#include "core/logger.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <fstream>
#include <memory>

namespace wowee::core {
namespace fs = std::filesystem;

namespace {

// The locales ezLib recognises in a patch name, as its own pattern lists them.
constexpr auto kStallTimeout = std::chrono::seconds(30);
constexpr int kMaxStallRetries = 3;

constexpr const char* kLocales[] = {"enUS", "enGB", "koKR", "frFR", "deDE",
                                    "zhCN", "zhTW", "esES", "esMX", "ruRU"};

std::string hex(const std::array<uint8_t, 16>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::optional<std::array<uint8_t, 16>> unhex(const std::string& text) {
    if (text.size() != 32) return std::nullopt;
    std::array<uint8_t, 16> out{};
    for (size_t i = 0; i < 32; ++i) {
        const char c = text[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return std::nullopt;
        out[i / 2] = static_cast<uint8_t>(out[i / 2] << 4 | v);
    }
    return out;
}

int64_t mtimeOf(const fs::path& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

fs::path partialPath(const fs::path& path) {
    auto p = path;
    p += ".partial";
    return p;
}

std::FILE* openForWrite(const fs::path& path, bool append) {
#ifdef _WIN32
    // The per-user folder can carry a non-ASCII account name.
    return _wfopen(path.wstring().c_str(), append ? L"ab" : L"wb");
#else
    return std::fopen(path.string().c_str(), append ? "ab" : "wb");
#endif
}

} // namespace

std::optional<std::array<uint8_t, 16>> md5OfFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || !EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr)) return std::nullopt;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got > 0 && !EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(got)))
            return std::nullopt;
    }
    if (in.bad()) return std::nullopt;
    std::array<uint8_t, 16> out{};
    unsigned size = 0;
    if (!EVP_DigestFinal_ex(ctx.get(), out.data(), &size) || size != out.size()) return std::nullopt;
    return out;
}

std::vector<RealmPatchPlanEntry> planRealmPatches(const std::vector<game::EzWowPatch>& manifest,
                                                  const std::string& locale) {
    std::vector<RealmPatchPlanEntry> plan;
    for (const auto& patch : manifest) {
        // "<number>[_lazy]_<rest>" - the number is the load order.
        const auto& name = patch.name;
        size_t digits = 0;
        while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9') ++digits;
        if (digits == 0 || digits > 6) {
            LOG_WARNING("Realm patch '", name, "' has no load-order prefix; ignored");
            continue;
        }
        size_t rest = digits;
        if (name.compare(rest, 5, "_lazy") == 0) rest += 5;
        if (rest >= name.size() || name[rest] != '_') {
            LOG_WARNING("Realm patch '", name, "' has no load-order prefix; ignored");
            continue;
        }
        RealmPatchPlanEntry entry;
        entry.patch = patch;
        entry.order = std::stoi(name.substr(0, digits));
        entry.localName = name.substr(rest + 1);
        if (!isSafePatchFileName(entry.localName)) {
            LOG_WARNING("Realm patch '", name, "' is not a plain archive name; ignored");
            continue;
        }

        const auto stem = entry.localName.substr(0, entry.localName.rfind('.'));
        const char* suffix = nullptr;
        for (const char* candidate : kLocales) {
            const std::string code(candidate);
            if (stem.size() > code.size() && stem.compare(stem.size() - code.size(), code.size(), code) == 0) {
                suffix = candidate;
                break;
            }
        }
        if (suffix && locale != suffix) continue;
        plan.push_back(std::move(entry));
    }
    std::stable_sort(plan.begin(), plan.end(),
                     [](const auto& a, const auto& b) { return a.order < b.order; });
    return plan;
}

RealmPatchSync::RealmPatchSync(fs::path directory, Transport transport)
    : directory_(std::move(directory)), transport_(std::move(transport)) {}

RealmPatchSync::~RealmPatchSync() {
    if (state_ == State::Downloading && transport_.cancel && transport_.connected && transport_.connected())
        transport_.cancel();
    closeFile();
    // Workers only read files and return values; waiting keeps them from
    // outliving the paths they were handed.
    if (checking_.valid()) checking_.wait();
    if (verifying_.valid()) verifying_.wait();
}

void RealmPatchSync::start(const std::vector<game::EzWowPatch>& manifest, const std::string& locale) {
    if (busy()) return;
    plan_ = planRealmPatches(manifest, locale);
    queue_.clear();
    queuePos_ = 0;
    completedBytes_ = totalBytes_ = 0;
    error_.clear();

    std::error_code ec;
    fs::create_directories(directory_, ec);
    if (ec) {
        fail("Cannot create " + directory_.string() + ": " + ec.message());
        return;
    }
    loadCache();
    LOG_INFO("Realm patches: ", plan_.size(), " of ", manifest.size(), " archives apply to locale ",
             locale, "; checking ", directory_.string());

    state_ = State::Checking;
    checking_ = std::async(std::launch::async, [plan = plan_, dir = directory_, sums = sums_]() mutable {
        CheckResult result;
        for (size_t i = 0; i < plan.size(); ++i) {
            const auto& entry = plan[i];
            const auto path = dir / entry.localName;
            std::error_code sizeEc;
            const auto size = fs::file_size(path, sizeEc);
            if (sizeEc || size != entry.patch.size) {
                result.missing.push_back(i);
                continue;
            }
            const auto mtime = mtimeOf(path);
            auto cached = sums.find(entry.localName);
            std::optional<std::array<uint8_t, 16>> md5;
            if (cached != sums.end() && cached->second.size == size && cached->second.mtime == mtime) {
                md5 = cached->second.md5;
            } else {
                md5 = md5OfFile(path);
                if (md5) sums[entry.localName] = {size, mtime, *md5};
            }
            if (!md5 || *md5 != entry.patch.checksum) result.missing.push_back(i);
        }
        result.sums = std::move(sums);
        return result;
    });
}

void RealmPatchSync::update() {
    if (state_ == State::Checking) {
        if (checking_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto result = checking_.get();
        sums_ = std::move(result.sums);
        saveCache();
        queue_ = std::move(result.missing);
        for (size_t index : queue_) totalBytes_ += plan_[index].patch.size;
        if (queue_.empty()) {
            LOG_INFO("Realm patches: all ", plan_.size(), " archives are current");
            state_ = State::Done;
            return;
        }
        LOG_INFO("Realm patches: ", queue_.size(), " archive(s) to download, ", totalBytes_, " bytes");
        beginNext();
        return;
    }

    if (state_ == State::Downloading) {
        if (transport_.connected && !transport_.connected()) {
            fail("The login server closed the connection during the download; it resumes next login");
            return;
        }
        if (std::chrono::steady_clock::now() - lastData_ > kStallTimeout) {
            const auto& entry = plan_[queue_[queuePos_]];
            if (stallRetries_ >= kMaxStallRetries) {
                fail("The login server stopped sending " + entry.localName + "; it resumes next login");
                return;
            }
            ++stallRetries_;
            if (std::fflush(file_) != 0 || !transport_.request || !transport_.request(entry.patch.name, received_)) {
                fail("Could not ask for the rest of " + entry.localName + "; it resumes next login");
                return;
            }
            lastData_ = std::chrono::steady_clock::now();
            LOG_WARNING("Realm patches: no data for ", kStallTimeout.count(), " s; asked for ",
                        entry.patch.name, " again from ", received_, " (retry ", stallRetries_, ")");
        }
        return;
    }

    if (state_ == State::Verifying) {
        if (verifying_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const auto md5 = verifying_.get();
        const auto& entry = plan_[queue_[queuePos_]];
        const auto finalPath = directory_ / entry.localName;
        const auto partial = partialPath(finalPath);
        std::error_code ec;
        if (!md5 || *md5 != entry.patch.checksum) {
            // A whole file that hashes wrong cannot be repaired by resuming it.
            fs::remove(partial, ec);
            fail(entry.localName + " arrived damaged (MD5 mismatch); it will be fetched again next login");
            return;
        }
        fs::remove(finalPath, ec);
        fs::rename(partial, finalPath, ec);
        if (ec) {
            fail("Cannot move " + entry.localName + " into place: " + ec.message());
            return;
        }
        sums_[entry.localName] = {entry.patch.size, mtimeOf(finalPath), *md5};
        saveCache();
        completedBytes_ += entry.patch.size;
        LOG_INFO("Realm patch ready: ", entry.localName);
        ++queuePos_;
        beginNext();
    }
}

void RealmPatchSync::beginNext() {
    if (queuePos_ >= queue_.size()) {
        LOG_INFO("Realm patches: download complete");
        state_ = State::Done;
        return;
    }
    const auto& entry = plan_[queue_[queuePos_]];
    const auto partial = partialPath(directory_ / entry.localName);

    std::error_code ec;
    uint64_t held = fs::exists(partial, ec) ? fs::file_size(partial, ec) : 0;
    if (ec || held > entry.patch.size) {
        fs::remove(partial, ec);
        held = 0;
    }
    received_ = held;

    if (held == entry.patch.size) {
        // Everything already arrived last time; only the check was missing.
        state_ = State::Verifying;
        verifying_ = std::async(std::launch::async, [partial] { return md5OfFile(partial); });
        return;
    }

    file_ = openForWrite(partial, held != 0);
    if (!file_) {
        fail("Cannot write " + partial.string());
        return;
    }
    if (!transport_.request || !transport_.request(entry.patch.name, held)) {
        closeFile();
        fail("The login connection is not available for downloading " + entry.localName);
        return;
    }
    state_ = State::Downloading;
    lastData_ = std::chrono::steady_clock::now();
    stallRetries_ = 0;
    LOG_INFO("Realm patch download: ", entry.patch.name, " (", entry.patch.size, " bytes, from ", held, ")");
}

void RealmPatchSync::onData(const uint8_t* data, size_t size) {
    if (state_ != State::Downloading || !file_) {
        LOG_WARNING("Realm patches: ", size, " transfer bytes arrived with no download in progress");
        return;
    }
    const auto& entry = plan_[queue_[queuePos_]];
    if (received_ + size > entry.patch.size) {
        closeFile();
        std::error_code ec;
        fs::remove(partialPath(directory_ / entry.localName), ec);
        if (transport_.cancel) transport_.cancel();
        fail("The server sent more of " + entry.localName + " than it announced");
        return;
    }
    if (size && std::fwrite(data, 1, size, file_) != size) {
        closeFile();
        if (transport_.cancel) transport_.cancel();
        fail("Writing " + entry.localName + " failed - is the disk full?");
        return;
    }
    received_ += size;
    lastData_ = std::chrono::steady_clock::now();
    if (size) stallRetries_ = 0;
    if (received_ < entry.patch.size) return;

    closeFile();
    state_ = State::Verifying;
    verifying_ = std::async(std::launch::async,
                            [partial = partialPath(directory_ / entry.localName)] { return md5OfFile(partial); });
}

RealmPatchSync::Progress RealmPatchSync::progress() const {
    Progress p;
    p.state = state_;
    p.error = error_;
    p.fileCount = queue_.size();
    p.totalBytes = totalBytes_;
    p.doneBytes = completedBytes_;
    if (queuePos_ < queue_.size()) {
        p.current = plan_[queue_[queuePos_]].localName;
        p.fileIndex = queuePos_ + 1;
        if (state_ == State::Downloading || state_ == State::Verifying) p.doneBytes += received_;
    }
    return p;
}

std::vector<fs::path> RealmPatchSync::archives() const {
    std::vector<fs::path> out;
    out.reserve(plan_.size());
    for (const auto& entry : plan_) out.push_back(directory_ / entry.localName);
    return out;
}

void RealmPatchSync::fail(const std::string& reason) {
    // Flush what arrived so a later run can resume from it.
    closeFile();
    LOG_ERROR("Realm patches: ", reason);
    error_ = reason;
    state_ = State::Failed;
}

void RealmPatchSync::closeFile() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void RealmPatchSync::loadCache() {
    sums_.clear();
    std::ifstream in(directory_ / "checksums.json");
    if (!in) return;
    try {
        const auto doc = nlohmann::json::parse(in);
        for (const auto& [name, value] : doc.items()) {
            if (!isSafePatchFileName(name)) continue;
            const auto md5 = unhex(value.at("md5").get<std::string>());
            if (!md5) continue;
            sums_[name] = {value.at("size").get<uint64_t>(), value.at("mtime").get<int64_t>(), *md5};
        }
    } catch (const std::exception& e) {
        // Only a cache: losing it costs one rehash.
        LOG_WARNING("Realm patches: ignoring unreadable checksum cache: ", e.what());
        sums_.clear();
    }
}

void RealmPatchSync::saveCache() const {
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [name, sum] : sums_)
        doc[name] = {{"size", sum.size}, {"mtime", sum.mtime}, {"md5", hex(sum.md5)}};
    const auto path = directory_ / "checksums.json";
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return;
        out << doc.dump(1);
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
}

} // namespace wowee::core
