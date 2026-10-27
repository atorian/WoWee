#include "title/domain/titles.hpp"

namespace wowee::title {

std::vector<TitleChange> Titles::earned(uint32_t bit, bool lost) {
    if (lost) known_.erase(bit); else known_.insert(bit);
    return {KnownTitlesChanged{}};
}

std::vector<TitleChange> Titles::updated(uint8_t operation, const std::vector<uint16_t>& bits) {
    if (operation < 1 || operation > 3) return {};
    if (operation == 3) known_.clear();
    for (auto bit : bits) {
        if (operation == 2) known_.erase(bit); else known_.insert(bit);
    }
    return {KnownTitlesChanged{}};
}

} // namespace wowee::title
