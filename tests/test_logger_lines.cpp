// A block of lines written by one thread stays together in the log while
// another thread is logging.
//
// The frame profile's report is a hundred and more lines, written on the
// background thread while the game thread goes on logging. Line by line, the
// game's own warnings landed in the middle of it.
#include "catch_amalgamated.hpp"
#include "core/logger.hpp"
#include "scratch_dir.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using wowee::core::Logger;
using wowee::core::LogLevel;

namespace {

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

TEST_CASE("a block of lines is not broken up by another thread's lines", "[logger]") {
    // The logger opens its file on first use, in the working directory, as
    // the environment says. Nothing in this process has logged yet.
    const std::filesystem::path dir = wowee::test::scratchDir("logger_lines");
    const std::filesystem::path startedIn = std::filesystem::current_path();
    std::filesystem::current_path(dir);
    setEnvironment("WOWEE_LOG_FILE", "lines.log");
    setEnvironment("WOWEE_LOG_STDOUT", "0");
    setEnvironment("WOWEE_LOG_FLUSH_MS", "0");
    Logger& logger = Logger::getInstance();

    constexpr int kBlockLines = 5000;
    std::vector<std::string> block;
    block.reserve(kBlockLines);
    for (int i = 0; i < kBlockLines; ++i) block.push_back("block line " + std::to_string(i));

    // Another thread logs as fast as it can for as long as the block is
    // being written.
    std::atomic<bool> otherStarted{false};
    std::atomic<bool> blockWritten{false};
    std::thread other([&] {
        for (int k = 0; !blockWritten.load(); ++k) {
            logger.log(LogLevel::WARNING, "other line " + std::to_string(k));
            otherStarted.store(true);
        }
    });
    while (!otherStarted.load()) std::this_thread::yield();
    logger.logLines(LogLevel::WARNING, block);
    blockWritten.store(true);
    other.join();
    std::filesystem::current_path(startedIn);

    std::ifstream file(dir / "logs" / "lines.log");
    REQUIRE(file.is_open());
    std::vector<int> blockAt;
    std::string line;
    for (int n = 0; std::getline(file, line); ++n) {
        if (line.find("block line ") != std::string::npos) blockAt.push_back(n);
    }
    // Every line of the block, each once, in order, with nothing between them.
    REQUIRE(blockAt.size() == static_cast<size_t>(kBlockLines));
    CHECK(blockAt.back() - blockAt.front() == kBlockLines - 1);
}
