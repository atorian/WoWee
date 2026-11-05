#pragma once

#include <cstddef>
#include <cstdint>

namespace wowee {
namespace core {

/// How short the machine is of memory, from least to most.
enum class MemoryPressure {
    None,
    /// Under 15% of RAM free: background loading slows to one item at a time.
    Low,
    /// Under 10% free: what still loads waits longer between items, so the
    /// frees of the work already in flight land before anything else is taken.
    Severe,
};

/**
 * Monitors system memory
 */
class MemoryMonitor {
public:
    static MemoryMonitor& getInstance();

    /// The pressure `availableBytes` free out of `totalBytes` is. Severe is
    /// the stricter: every machine that is severely short is also short.
    [[nodiscard]] static MemoryPressure classify(size_t availableBytes, size_t totalBytes);

    /**
     * Initialize memory monitoring
     */
    void initialize();

    /**
     * Get total system RAM in bytes
     */
    [[nodiscard]] size_t getTotalRAM() const { return totalRAM_; }

    /**
     * Get currently available RAM in bytes
     */
    [[nodiscard]] size_t getAvailableRAM() const;

    /// The pressure the machine is under now, from one reading of the free RAM.
    [[nodiscard]] MemoryPressure pressure() const;

private:
    MemoryMonitor() = default;
    size_t totalRAM_ = 0;
};

} // namespace core
} // namespace wowee
