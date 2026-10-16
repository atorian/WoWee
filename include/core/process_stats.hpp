#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace wowee {
namespace core {

/// What the operating system says of this process and of the machine's
/// memory. A figure the platform does not report, or failed to, is empty.
struct ProcessStats {
    /// Physical memory the process has resident.
    std::optional<uint64_t> workingSetBytes;
    /// Memory the process has committed for itself - what it would need
    /// backed by RAM or the page file if all of it were touched. On Windows
    /// the commit charge; on Linux the private writable mappings (VmData plus
    /// VmStk); on macOS the physical footprint.
    std::optional<uint64_t> privateBytes;
    /// Committed memory across the whole system, and the most it can commit:
    /// the RAM plus the page file. An allocation fails once the one reaches
    /// the other. Not reported on macOS, which does not limit commit.
    std::optional<uint64_t> systemCommitUsedBytes;
    std::optional<uint64_t> systemCommitLimitBytes;
    std::optional<uint64_t> availablePhysicalBytes;
    std::optional<uint64_t> totalPhysicalBytes;
    /// Threads the process runs.
    std::optional<uint32_t> threadCount;
};

/// Reads ProcessStats, saying once per kind of figure when the platform
/// cannot give it.
class ProcessStatsReader {
public:
    [[nodiscard]] ProcessStats read();

private:
    bool saidProcessMemory_ = false;
    bool saidSystemMemory_ = false;
    bool saidThreads_ = false;
};

/// A thread that reads ProcessStats once a second, so the frame never waits
/// on it: counting the process's threads on Windows walks every process on
/// the machine and took from 17 to 180 milliseconds on a loaded one.
class ProcessStatsSampler {
public:
    ProcessStatsSampler();
    ~ProcessStatsSampler();
    ProcessStatsSampler(const ProcessStatsSampler&) = delete;
    ProcessStatsSampler& operator=(const ProcessStatsSampler&) = delete;

    /// The latest reading, or empty before the first one (or when the
    /// sampler could not start, which it said).
    [[nodiscard]] std::optional<ProcessStats> latest() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::optional<ProcessStats> latest_;
    std::thread thread_;
};

} // namespace core
} // namespace wowee
