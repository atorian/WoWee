#include "guild/domain/guild_names.hpp"

namespace wowee::guild {

void GuildNames::restore(std::unordered_map<uint32_t, std::string> names) {
    for (auto& [id, name] : names)
        if (id != 0 && !name.empty()) names_[id] = std::move(name);
}

const std::string& GuildNames::find(uint32_t guildId) const {
    static const std::string kNone;
    auto it = names_.find(guildId);
    return it != names_.end() ? it->second : kNone;
}

bool GuildNames::shouldQuery(uint32_t guildId, Clock::time_point now) {
    if (guildId == 0 || names_.count(guildId)) return false;
    auto it = asked_.find(guildId);
    if (it != asked_.end() && now - it->second < std::chrono::seconds(2)) return false;
    asked_[guildId] = now;
    return true;
}

bool GuildNames::learn(uint32_t guildId, const std::string& name) {
    if (guildId == 0 || name.empty()) return false;
    auto it = names_.find(guildId);
    if (it != names_.end() && it->second == name) return false;
    names_[guildId] = name;
    return true;
}

void GuildNames::answered(uint32_t guildId) {
    asked_.erase(guildId);
}

} // namespace wowee::guild
