#pragma once

#include "core/frame_plan.hpp"
#include "core/logger.hpp"

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace wowee::core {

/// Nested loading frames report their own phase, then restore their caller's.
class ScopedPhase {
public:
    ScopedPhase(Phase& current, Phase next) : current_(current), previous_(current) { current_ = next; }
    ~ScopedPhase() { current_ = previous_; }
    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;
private:
    Phase& current_;
    Phase previous_;
};

[[nodiscard]] inline bool requirePhase(Phase current, Phase expected, const char* operation) {
    if (current == expected) return true;
    static std::mutex mutex;
    static std::set<std::pair<std::string, Phase>> reported;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (reported.emplace(operation, current).second) {
            LOG_ERROR(operation, " requires phase ", phaseName(expected),
                      "; current phase is ", phaseName(current));
        }
    }
#if !defined(NDEBUG) || (defined(WOWEE_PHASE_CHECKS_FATAL) && WOWEE_PHASE_CHECKS_FATAL)
    std::abort();
#endif
    return false;
}

} // namespace wowee::core
