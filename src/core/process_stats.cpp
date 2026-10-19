#include "core/process_stats.hpp"

#include "core/env_flag.hpp"
#include "core/logger.hpp"

#include <chrono>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace wowee::core {

namespace {

#ifdef _WIN32

void readProcessMemory(ProcessStats& stats, bool& said) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        stats.workingSetBytes = counters.WorkingSetSize;
        stats.privateBytes = counters.PrivateUsage;
    } else if (!said) {
        said = true;
        LOG_WARNING("Process memory not read: GetProcessMemoryInfo failed (error ", GetLastError(),
                    "); the performance HUD and the resource log show no working set or "
                    "private commit");
    }
}

void readSystemMemory(ProcessStats& stats, bool& said) {
    PERFORMANCE_INFORMATION info{};
    info.cb = sizeof(info);
    if (K32GetPerformanceInfo(&info, sizeof(info))) {
        const uint64_t page = info.PageSize;
        stats.systemCommitUsedBytes = static_cast<uint64_t>(info.CommitTotal) * page;
        stats.systemCommitLimitBytes = static_cast<uint64_t>(info.CommitLimit) * page;
        stats.availablePhysicalBytes = static_cast<uint64_t>(info.PhysicalAvailable) * page;
        stats.totalPhysicalBytes = static_cast<uint64_t>(info.PhysicalTotal) * page;
    } else if (!said) {
        said = true;
        LOG_WARNING("System memory not read: GetPerformanceInfo failed (error ", GetLastError(),
                    "); the performance HUD and the resource log show no system commit or "
                    "physical memory");
    }
}

void readThreads(ProcessStats& stats, bool& said) {
    // The process's own entry in a snapshot of the processes: Windows has no
    // per-process call for its thread count. A thread snapshot would list
    // every thread on the machine instead, and takes ten times as long.
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (!said) {
            said = true;
            LOG_WARNING("Thread count not read: CreateToolhelp32Snapshot failed (error ",
                        GetLastError(), "); the performance HUD and the resource log show none");
        }
        return;
    }
    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32First(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self) {
                stats.threadCount = entry.cntThreads;
                found = true;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!found && !said) {
        said = true;
        LOG_WARNING("Thread count not read: this process (", self, ") is not in the process "
                    "snapshot; the performance HUD and the resource log show none");
    }
}

void readHeaps(ProcessStats& stats, bool& said) {
    HANDLE heaps[256];
    const DWORD count = GetProcessHeaps(256, heaps);
    if (count == 0 || count > 256) {
        if (!said) {
            said = true;
            LOG_WARNING("Heap summary not read: GetProcessHeaps answered ", count,
                        " (error ", GetLastError(), "); the resource log shows no heap figures");
        }
        return;
    }
    uint64_t allocated = 0, committed = 0;
    for (DWORD i = 0; i < count; ++i) {
        HEAP_SUMMARY summary{};
        summary.cb = sizeof(summary);
        if (!HeapSummary(heaps[i], 0, &summary)) continue;
        allocated += summary.cbAllocated;
        committed += summary.cbCommitted;
    }
    stats.heapAllocatedBytes = allocated;
    stats.heapCommittedBytes = committed;
}

#elif defined(__APPLE__)

void readProcessMemory(ProcessStats& stats, bool& said) {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const kern_return_t result =
        task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    if (result == KERN_SUCCESS) {
        stats.workingSetBytes = info.resident_size;
        stats.privateBytes = info.phys_footprint;
    } else if (!said) {
        said = true;
        LOG_WARNING("Process memory not read: task_info(TASK_VM_INFO) failed (", result,
                    "); the performance HUD and the resource log show no resident or "
                    "footprint size");
    }
}

void readSystemMemory(ProcessStats& stats, bool& said) {
    int64_t physical = 0;
    size_t length = sizeof(physical);
    const bool haveTotal = sysctlbyname("hw.memsize", &physical, &length, nullptr, 0) == 0;
    if (haveTotal) stats.totalPhysicalBytes = static_cast<uint64_t>(physical);

    mach_port_t host = mach_host_self();
    vm_size_t pageSize = 0;
    host_page_size(host, &pageSize);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const bool haveVm = pageSize > 0 &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) ==
            KERN_SUCCESS;
    // Free and inactive pages are what the system hands out on demand.
    if (haveVm) {
        stats.availablePhysicalBytes =
            (static_cast<uint64_t>(vm.free_count) + vm.inactive_count) * pageSize;
    }
    if (!said) {
        said = true;
        // macOS does not limit commit: it compresses and swaps instead, so
        // there is no used-of-limit to show. Said once, so an empty figure is
        // not taken for a failure.
        LOG_WARNING("System commit is not reported on macOS (it has no commit limit); the "
                    "performance HUD and the resource log show none",
                    haveTotal && haveVm ? "" : "; physical memory not read either");
    }
}

void readThreads(ProcessStats& stats, bool& said) {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    const kern_return_t result = task_threads(mach_task_self(), &threads, &count);
    if (result == KERN_SUCCESS) {
        stats.threadCount = count;
        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                      count * sizeof(thread_act_t));
    } else if (!said) {
        said = true;
        LOG_WARNING("Thread count not read: task_threads failed (", result,
                    "); the performance HUD and the resource log show none");
    }
}

#else

/// The "Name:   1234 kB" fields of a /proc file, in bytes; a field that is
/// not a size (Threads) is returned as it reads.
bool readProcFields(const char* path, std::initializer_list<std::pair<const char*, uint64_t*>> fields) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string line;
    size_t found = 0;
    while (std::getline(file, line)) {
        for (const auto& [name, out] : fields) {
            const std::string prefix = std::string(name) + ":";
            if (line.rfind(prefix, 0) != 0) continue;
            std::istringstream value(line.substr(prefix.size()));
            uint64_t number = 0;
            std::string unit;
            value >> number >> unit;
            *out = unit == "kB" ? number * 1024ull : number;
            ++found;
        }
    }
    return found == fields.size();
}

void readProcessMemory(ProcessStats& stats, bool& said) {
    uint64_t rss = 0, data = 0, stack = 0;
    if (readProcFields("/proc/self/status", {{"VmRSS", &rss}, {"VmData", &data}, {"VmStk", &stack}})) {
        stats.workingSetBytes = rss;
        stats.privateBytes = data + stack;
    } else if (!said) {
        said = true;
        LOG_WARNING("Process memory not read: /proc/self/status has no VmRSS, VmData or VmStk; "
                    "the performance HUD and the resource log show no resident or private size");
    }
}

void readSystemMemory(ProcessStats& stats, bool& said) {
    uint64_t total = 0, available = 0, limit = 0, committed = 0;
    if (readProcFields("/proc/meminfo", {{"MemTotal", &total}, {"MemAvailable", &available},
                                         {"CommitLimit", &limit}, {"Committed_AS", &committed}})) {
        stats.totalPhysicalBytes = total;
        stats.availablePhysicalBytes = available;
        stats.systemCommitLimitBytes = limit;
        stats.systemCommitUsedBytes = committed;
    } else if (!said) {
        said = true;
        LOG_WARNING("System memory not read: /proc/meminfo lacks MemTotal, MemAvailable, "
                    "CommitLimit or Committed_AS; the performance HUD and the resource log show "
                    "no system commit or physical memory");
    }
}

void readThreads(ProcessStats& stats, bool& said) {
    uint64_t threads = 0;
    if (readProcFields("/proc/self/status", {{"Threads", &threads}})) {
        stats.threadCount = static_cast<uint32_t>(threads);
    } else if (!said) {
        said = true;
        LOG_WARNING("Thread count not read: /proc/self/status has no Threads; the performance "
                    "HUD and the resource log show none");
    }
}

#endif

#ifndef _WIN32
void readHeaps(ProcessStats&, bool& said) {
    if (said) return;
    said = true;
    LOG_WARNING("WOWEE_HEAP_SUMMARY: heap figures are read on Windows only; the resource log "
                "shows none here");
}
#endif

} // namespace

ProcessStats ProcessStatsReader::read(bool withThreads, bool withHeaps) {
    ProcessStats stats;
    readProcessMemory(stats, saidProcessMemory_);
    readSystemMemory(stats, saidSystemMemory_);
    if (withThreads) readThreads(stats, saidThreads_);
    if (withHeaps) readHeaps(stats, saidHeaps_);
    return stats;
}

ProcessStatsSampler::ProcessStatsSampler() {
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error& e) {
        LOG_ERROR("Process stats sampler not started: ", e.what(),
                  "; the performance HUD and the resource log show no memory or thread figures");
    }
}

ProcessStatsSampler::~ProcessStatsSampler() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::optional<ProcessStats> ProcessStatsSampler::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void ProcessStatsSampler::run() {
    constexpr int kThreadCountEvery = 5;
    constexpr int kHeapSummaryEvery = 10;
    static const bool heapSummary = envFlagEnabled("WOWEE_HEAP_SUMMARY", false);
    if (heapSummary) {
        LOG_WARNING("WOWEE_HEAP_SUMMARY: the process's heaps are summed every ",
                    kHeapSummaryEvery, " s; each summary walks every block and holds the heap "
                    "while it does");
    }
    ProcessStatsReader reader;
    std::optional<uint32_t> threads;
    std::optional<uint64_t> heapAllocated, heapCommitted;
    int reading = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        lock.unlock();
        const bool withThreads = reading % kThreadCountEvery == 0;
        const bool withHeaps = heapSummary && reading % kHeapSummaryEvery == 0;
        ++reading;
        ProcessStats stats = reader.read(withThreads, withHeaps);
        if (withThreads) threads = stats.threadCount; else stats.threadCount = threads;
        if (withHeaps) {
            heapAllocated = stats.heapAllocatedBytes;
            heapCommitted = stats.heapCommittedBytes;
        } else {
            stats.heapAllocatedBytes = heapAllocated;
            stats.heapCommittedBytes = heapCommitted;
        }
        lock.lock();
        latest_ = std::move(stats);
        wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; });
    }
}

} // namespace wowee::core
