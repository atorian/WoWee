#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee::guild {

/// Names of guilds other than the player's, by id, for nameplates and
/// tooltips. A name is asked for once and then remembered across sessions.
class GuildNames {
public:
    using Clock = std::chrono::steady_clock;

    /// Names remembered from earlier sessions.
    void restore(std::unordered_map<uint32_t, std::string> names);

    /// The name, or empty while it is unknown.
    [[nodiscard]] const std::string& find(uint32_t guildId) const;

    /// Whether to ask the server for an unknown name now. An answer can be
    /// lost, so the question is asked again after two seconds - but not on
    /// every frame a nameplate draws.
    bool shouldQuery(uint32_t guildId, Clock::time_point now);

    /// Returns whether this changed what is remembered.
    bool learn(uint32_t guildId, const std::string& name);
    void answered(uint32_t guildId);

    [[nodiscard]] const std::unordered_map<uint32_t, std::string>& all() const { return names_; }

private:
    std::unordered_map<uint32_t, std::string> names_;
    std::unordered_map<uint32_t, Clock::time_point> asked_;
};

} // namespace wowee::guild
