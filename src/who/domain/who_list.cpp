#include "who/domain/who_list.hpp"

#include <utility>

namespace wowee::who {

std::vector<WhoChange> WhoList::answered(uint32_t found, uint32_t onlineCount, std::vector<WhoEntry> entries) {
    results_ = std::move(entries);
    onlineCount_ = onlineCount;
    if (found == 0) return {NoPlayersFound{}};
    std::vector<WhoChange> changes;
    if (!toUI_) changes.emplace_back(WhoPrinted{results_, onlineCount_});
    changes.emplace_back(WhoListUpdated{});
    return changes;
}

void WhoList::beginCharacter() {
    results_.clear();
    onlineCount_ = 0;
}

} // namespace wowee::who
