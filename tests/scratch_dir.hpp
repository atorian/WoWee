#pragma once

/// Scratch directories for tests: what a test writes to disk goes here.
///
/// Apart from test_support.hpp so that a child process a test starts, which
/// has no Catch2, asks for its scratch the way the test does.

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace wowee::test {

/// Where this process keeps its scratch directories: a directory of its own
/// under the temp directory, removed when the process exits.
///
/// Of its own because ctest runs each file of a suite as a process of the
/// same executable, many at once, and every harness asks for the same names.
/// Shared, each process emptied the others' scratch on its way in - their
/// settings and saved variables went, or creating a directory failed between
/// another's removal and its own creation.
///
/// The temp directory is TMP/TEMP/TMPDIR, which a build on a full system drive
/// points somewhere with room; nothing here names a drive.
inline const std::filesystem::path& processScratchRoot() {
    struct Root {
        std::filesystem::path path;
        Root() {
#ifdef _WIN32
            const auto pid = _getpid();
#else
            const auto pid = getpid();
#endif
            path = std::filesystem::temp_directory_path() / ("wowee_test_" + std::to_string(pid));
            // A process that died before it could remove its own, under an id
            // this one has been given again.
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        ~Root() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };
    static const Root root;
    return root.path;
}

/// An empty directory of that name in this process's scratch, made fresh: what
/// an earlier test of the process left there is removed first. A directory
/// that cannot be made throws, naming it.
inline std::filesystem::path scratchDir(const char* name) {
    const std::filesystem::path dir = processScratchRoot() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace wowee::test
