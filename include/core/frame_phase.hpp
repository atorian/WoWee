#pragma once

#include "core/frame_plan.hpp"
#include "core/logger.hpp"

#include <stdexcept>
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

inline void requirePhase(Phase current, Phase expected, const char* operation) {
    if (current == expected) return;
    const std::string error = std::string(operation) + " requires phase " + phaseName(expected) +
                              "; current phase is " + phaseName(current);
    LOG_ERROR(error);
    throw std::logic_error(error);
}

inline void requireLuaPhase(Phase current, const char* operation) {
    if (current != Phase::Render) return;
    const std::string error = std::string("Lua execution during Render: ") + operation;
    LOG_ERROR(error);
    throw std::logic_error(error);
}

} // namespace wowee::core
