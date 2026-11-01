#include "core/scene_capture_files.hpp"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace wowee::core {
namespace {
// std::filesystem::last_write_time is not used: libstdc++ on Windows reads it
// through the C runtime's local-time conversion, and the value it gives for an
// unchanged file moves by an hour when daylight saving starts or ends.
int64_t writtenUtcNs(const std::filesystem::path& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        throw std::runtime_error("cannot read the write time of " + path.string() +
            " (Windows error " + std::to_string(GetLastError()) + ")");
    // FILETIME: 100 ns ticks since 1601-01-01 UTC.
    const auto ticks = static_cast<int64_t>((static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        data.ftLastWriteTime.dwLowDateTime);
    constexpr int64_t unixEpochTicks = 116444736000000000;
    return (ticks - unixEpochTicks) * 100;
#else
    struct stat info{};
    if (stat(path.c_str(), &info) != 0)
        throw std::runtime_error("cannot read the write time of " + path.string() + ": " +
            std::generic_category().message(errno));
#ifdef __APPLE__
    const auto& written = info.st_mtimespec;
#else
    const auto& written = info.st_mtim;
#endif
    return static_cast<int64_t>(written.tv_sec) * 1000000000 + written.tv_nsec;
#endif
}
}

nlohmann::json signCapturedFile(const std::filesystem::path& path) {
    return {{"path", path.string()}, {"bytes", std::filesystem::file_size(path)}, {"written", writtenUtcNs(path)}};
}

void verifyCapturedFile(const nlohmann::json& expected) {
    const auto path = expected.at("path").get<std::string>();
    if (!expected.contains("written"))
        throw std::runtime_error("capture signs " + path + " in the local-time form that moves with daylight "
            "saving; re-sign the capture with tools/resign_scene_capture.py");
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("capture asset unreadable: " + path + ": " + error.message());
    const auto written = writtenUtcNs(path);
    if (bytes != expected.at("bytes").get<uintmax_t>() || written != expected.at("written").get<int64_t>())
        throw std::runtime_error("capture asset changed: " + path + " (bytes " + expected.at("bytes").dump() +
            " -> " + std::to_string(bytes) + ", written " + expected.at("written").dump() + " -> " +
            std::to_string(written) + " ns UTC)");
}

void verifyCapturedDataPath(const std::string& current, const std::string& captured) {
    std::error_code error;
    const bool same = std::filesystem::equivalent(current, captured, error);
    if (error)
        throw std::runtime_error("cannot compare the replay data root " + current + " with the captured " +
            captured + ": " + error.message());
    if (!same)
        throw std::runtime_error("replay data root " + current + " is not the captured " + captured +
            "; use the captured dataPath");
}

}
