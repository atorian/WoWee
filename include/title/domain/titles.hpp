#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "title/domain/events.hpp"

namespace wowee::title {

/// The titles the player has, and the one worn.
class Titles {
public:
    std::vector<TitleChange> earned(uint32_t bit, bool lost);
    /// A server extension's list: 1 adds, 2 removes, 3 replaces. Anything else
    /// is ignored.
    std::vector<TitleChange> updated(uint8_t operation, const std::vector<uint16_t>& bits);
    /// PLAYER_CHOSEN_TITLE, or the player's own choice. -1 wears none.
    void wear(int32_t bit) { chosen_ = bit; }
    /// The player left the world: what the next character knows and wears is
    /// its own.
    void leftWorld() { *this = Titles{}; }

    [[nodiscard]] const std::unordered_set<uint32_t>& known() const { return known_; }
    [[nodiscard]] int32_t chosen() const { return chosen_; }

private:
    std::unordered_set<uint32_t> known_;
    int32_t chosen_ = -1;
};

} // namespace wowee::title
