#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace wowee::who {

struct WhoEntry {
    std::string name;
    std::string guildName;
    uint32_t level    = 0;
    uint32_t classId  = 0;
    uint32_t raceId   = 0;
    uint32_t zoneId   = 0;
};

/// A search found nobody.
struct NoPlayersFound {};
/// The rows, for the chat, when no panel is showing them.
struct WhoPrinted { std::vector<WhoEntry> entries; uint32_t onlineCount = 0; };
/// The who panel redraws its rows on this (WHO_LIST_UPDATE).
struct WhoListUpdated {};

using WhoChange = std::variant<NoPlayersFound, WhoPrinted, WhoListUpdated>;

/// The last /who answer, and where answers go: the panel or the chat.
class WhoList {
public:
    /// `found` is the count the server states; a search that found nobody
    /// says so and leaves the panel alone.
    std::vector<WhoChange> answered(uint32_t found, uint32_t onlineCount, std::vector<WhoEntry> entries);
    /// FriendsFrame turns this on while its Who tab is up and off again on the
    /// way out, so a search typed into chat with the panel closed prints there.
    void showInPanel(bool toUI) { toUI_ = toUI; }
    /// A character is logging in: the last one's answer is not its own.
    /// Where answers go is the interface's setting and stays.
    void beginCharacter();

    [[nodiscard]] const std::vector<WhoEntry>& results() const { return results_; }
    [[nodiscard]] uint32_t onlineCount() const { return onlineCount_; }
    [[nodiscard]] bool toUI() const { return toUI_; }

private:
    std::vector<WhoEntry> results_;
    uint32_t onlineCount_ = 0;
    bool toUI_ = false;
};

} // namespace wowee::who
