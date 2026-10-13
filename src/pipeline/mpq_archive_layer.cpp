#include "pipeline/mpq_archive_layer.hpp"
#include "core/logger.hpp"

#include <StormLib.h>

#include <mutex>

namespace wowee {
namespace pipeline {

namespace {

/// StormLib keeps state that is not safe to share across threads even between
/// separate handles (see the extractor), and the stack being replaced can still
/// be read while its successor opens, so one lock serves every layer.
std::mutex& stormMutex() {
    static std::mutex mutex;
    return mutex;
}

/// See lastStormError in tools/mpq_build: which spelling exists depends on
/// how StormLib was packaged, and CMake asks the header.
unsigned lastStormError() {
#ifdef WOWEE_STORMLIB_SERR
    return static_cast<unsigned>(SErrGetLastError());
#else
    return static_cast<unsigned>(GetLastError());
#endif
}

std::string normalize(std::string path) {
    for (char& c : path) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return path;
}

/// The archive's own bookkeeping - "(listfile)", "(attributes)",
/// "(signature)" - which no caller asks for and which every archive has, so
/// whichever came last would claim them.
bool isArchiveMetadata(const std::string& name) {
    return !name.empty() && name.front() == '(';
}

/// Larger than any file a 3.3.5 archive holds - the biggest realm DBC diff is
/// 60 MB - and small enough that a damaged size field cannot ask for the
/// whole address space.
constexpr DWORD kMaxFileBytes = 512u * 1024 * 1024;

/// A whole file out of an open archive, or nothing. Called under stormMutex.
std::vector<uint8_t> readLocked(HANDLE archive, const char* name) {
    HANDLE file = nullptr;
    if (!SFileOpenFileEx(archive, name, SFILE_OPEN_FROM_MPQ, &file)) return {};
    std::vector<uint8_t> data;
    const DWORD size = SFileGetFileSize(file, nullptr);
    if (size != SFILE_INVALID_SIZE && size > 0 && size <= kMaxFileBytes) {
        data.resize(size);
        DWORD got = 0;
        // False with everything read is the end of the file, not a failure.
        if (SFileReadFile(file, data.data(), size, &got, nullptr) || got == size) {
            data.resize(got);
        } else {
            data.clear();
        }
    }
    SFileCloseFile(file);
    return data;
}

} // namespace

std::shared_ptr<MpqArchiveLayer> MpqArchiveLayer::open(
    const std::vector<std::filesystem::path>& archives, std::string& error) {
    std::shared_ptr<MpqArchiveLayer> layer(new MpqArchiveLayer());
    for (const auto& path : archives) {
        // Taken per archive rather than for the whole stack, so the layer
        // still in use is not held up for the second this takes.
        std::lock_guard<std::mutex> lock(stormMutex());
        HANDLE handle = nullptr;
        // Without StormLib's own listfile pass, which names every file in its
        // tables at open and took 4.3 s over EZWoW's 24 archives; reading the
        // list here and asking after each name takes 0.2 s.
        if (!SFileOpenArchive(path.string().c_str(), 0, STREAM_FLAG_READ_ONLY | MPQ_OPEN_NO_LISTFILE,
                              &handle)) {
            error = "cannot open " + path.string() + " (StormLib error " +
                    std::to_string(lastStormError()) + ")";
            return nullptr;  // the destructor closes the ones already open
        }
        const auto index = static_cast<uint32_t>(layer->archives_.size());
        layer->archives_.push_back({handle, path});

        const std::vector<uint8_t> list = readLocked(handle, "(listfile)");
        if (list.empty()) {
            LOG_WARNING("Realm archive ", path.filename().string(),
                        " lists no files; nothing in it can be found");
            continue;
        }
        // One name per line; some tools separate them with ';' instead.
        std::string name;
        auto take = [&] {
            // A name the list carries but the archive does not hold - lists
            // are written by hand often enough - is not offered.
            if (!name.empty() && !isArchiveMetadata(name) && SFileHasFile(handle, name.c_str())) {
                // Assigned rather than emplaced: the later archive wins.
                layer->owner_[normalize(name)] = index;
            }
            name.clear();
        };
        for (uint8_t c : list) {
            if (c == '\r' || c == '\n' || c == ';' || c == 0) take();
            else name += static_cast<char>(c);
        }
        take();
    }
    return layer;
}

MpqArchiveLayer::~MpqArchiveLayer() {
    std::lock_guard<std::mutex> lock(stormMutex());
    for (auto& archive : archives_) {
        if (archive.handle) SFileCloseArchive(archive.handle);
    }
}

bool MpqArchiveLayer::contains(const std::string& normalizedPath) const {
    return owner_.find(normalizedPath) != owner_.end();
}

std::vector<uint8_t> MpqArchiveLayer::read(const std::string& normalizedPath) const {
    const auto it = owner_.find(normalizedPath);
    if (it == owner_.end()) return {};
    const Archive& archive = archives_[it->second];

    std::lock_guard<std::mutex> lock(stormMutex());
    // MPQ names hash case-insensitively and with backslashes, which is what a
    // normalized path already is.
    auto data = readLocked(archive.handle, normalizedPath.c_str());
    if (data.empty()) {
        LOG_WARNING("Realm archive ", archive.path.filename().string(), " lists ", normalizedPath,
                    " but it cannot be read (StormLib error ", lastStormError(), ")");
    }
    return data;
}

std::vector<size_t> MpqArchiveLayer::filesPerArchive() const {
    std::vector<size_t> counts(archives_.size(), 0);
    for (const auto& [path, index] : owner_) ++counts[index];
    return counts;
}

} // namespace pipeline
} // namespace wowee
