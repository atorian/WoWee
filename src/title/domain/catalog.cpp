#include "title/domain/catalog.hpp"

namespace wowee::title {

std::string formatTitle(const std::string& format, const std::string& name) {
    const size_t pos = format.find("%s");
    if (pos == std::string::npos) return format;
    return format.substr(0, pos) + name + format.substr(pos + 2);
}

void TitleCatalog::describe(const std::vector<TitleRecord>& rows) {
    for (const auto& r : rows) {
        if (r.format.empty()) continue;
        // By id for a quest reward (which carries the id), and by mask bit for
        // the worn title (which is a bit off the player's own fields).
        if (r.id != 0) byId_[r.id] = r.format;
        if (r.bit != 0) byBit_[r.bit] = r.format;
    }
}

std::string TitleCatalog::byBit(uint32_t bit, const std::string& name) const {
    auto it = byBit_.find(bit);
    return it != byBit_.end() ? formatTitle(it->second, name) : std::string();
}

std::string TitleCatalog::byId(uint32_t id, const std::string& name) const {
    auto it = byId_.find(id);
    return it != byId_.end() ? formatTitle(it->second, name) : std::string();
}

} // namespace wowee::title
