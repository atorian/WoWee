#include "core/memory_monitor.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <string>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

namespace wowee {
namespace core {

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {
size_t readMemAvailableBytesFromProc() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return 0;

    std::string line;
    while (std::getline(meminfo, line)) {
        // /proc/meminfo format: "MemAvailable:  123456789 kB"
        static constexpr size_t kFieldPrefixLen = 13; // strlen("MemAvailable:")
        if (line.rfind("MemAvailable:", 0) != 0) continue;
        std::istringstream iss(line.substr(kFieldPrefixLen));
        size_t kb = 0;
        iss >> kb;
        if (kb > 0) return kb * 1024ull;
        break;
    }
    return 0;
}
} // namespace
#endif // !_WIN32 && !__APPLE__

MemoryMonitor& MemoryMonitor::getInstance() {
    static MemoryMonitor instance;
    return instance;
}

void MemoryMonitor::initialize() {
    constexpr size_t kOneGB = 1024ull * 1024 * 1024;
    // Fallback if OS API unavailable - 16 GB is a safe conservative estimate
    // that prevents over-aggressive asset caching on unknown hardware.
    constexpr size_t kFallbackRAM = 16 * kOneGB;

#ifdef _WIN32
    ULONGLONG totalKB = 0;
    if (GetPhysicallyInstalledSystemMemory(&totalKB)) {
        totalRAM_ = static_cast<size_t>(totalKB) * 1024ull;
        LOG_INFO("System RAM detected: ", totalRAM_ / kOneGB, " GB");
    } else {
        totalRAM_ = kFallbackRAM;
        LOG_WARNING("Could not detect system RAM, assuming 16GB");
    }
#elif defined(__APPLE__)
    int64_t physmem = 0;
    size_t len = sizeof(physmem);
    if (sysctlbyname("hw.memsize", &physmem, &len, nullptr, 0) == 0) {
        totalRAM_ = static_cast<size_t>(physmem);
        LOG_INFO("System RAM detected: ", totalRAM_ / kOneGB, " GB");
    } else {
        totalRAM_ = kFallbackRAM;
        LOG_WARNING("Could not detect system RAM, assuming 16GB");
    }
#else
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        totalRAM_ = static_cast<size_t>(info.totalram) * info.mem_unit;
        LOG_INFO("System RAM detected: ", totalRAM_ / kOneGB, " GB");
    } else {
        totalRAM_ = kFallbackRAM;
        LOG_WARNING("Could not detect system RAM, assuming 16GB");
    }
#endif
}

size_t MemoryMonitor::getAvailableRAM() const {
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<size_t>(status.ullAvailPhys);
    }
    return totalRAM_ / 2;
#elif defined(__APPLE__)
    // hw.usermem is a 32-bit kernel sysctl on macOS: on systems with ≥16 GB RAM
    // the value overflows signed int32, truncating to ~2 GB and causing false
    // severe-pressure reports.  Use the Mach VM statistics API instead, which
    // is the same source that Activity Monitor and `vm_stat` use.
    {
        mach_port_t host_port = mach_host_self();
        vm_size_t page_size = 0;
        host_page_size(host_port, &page_size);
        if (page_size > 0) {
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
            vm_statistics64_data_t vm_stat;
            if (host_statistics64(host_port, HOST_VM_INFO64,
                                  reinterpret_cast<host_info64_t>(&vm_stat),
                                  &count) == KERN_SUCCESS) {
                // free + inactive pages are reclaimable on demand
                return (static_cast<size_t>(vm_stat.free_count) +
                        static_cast<size_t>(vm_stat.inactive_count)) *
                       static_cast<size_t>(page_size);
            }
        }
    }
    return totalRAM_ / 2;
#else
    // Best source on Linux for reclaimable memory headroom.
    if (size_t memAvailable = readMemAvailableBytesFromProc(); memAvailable > 0) {
        return memAvailable;
    }

    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        // Fallback approximation if /proc/meminfo is unavailable.
        size_t freeBytes = static_cast<size_t>(info.freeram) * info.mem_unit;
        size_t bufferBytes = static_cast<size_t>(info.bufferram) * info.mem_unit;
        size_t available = freeBytes + bufferBytes;
        return (totalRAM_ > 0 && available > totalRAM_) ? totalRAM_ : available;
    }
    return totalRAM_ / 2;  // Fallback: assume 50% available
#endif
}

MemoryPressure MemoryMonitor::classify(size_t availableBytes, size_t totalBytes) {
    // Severe was the looser of the two (under 15% free, against under 10% for
    // plain pressure), so it was the only one that ever fired: every machine
    // short of memory took the long wait, and the short one never ran.
    if (availableBytes < totalBytes / 100 * 10) return MemoryPressure::Severe;
    if (availableBytes < totalBytes / 100 * 15) return MemoryPressure::Low;
    return MemoryPressure::None;
}

MemoryPressure MemoryMonitor::pressure() const {
    return classify(getAvailableRAM(), totalRAM_);
}

} // namespace core
} // namespace wowee
