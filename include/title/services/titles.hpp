#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace wowee::title {

/// Titles as the interface and the addon layer use them. CharTitles.dbc is
/// read on the first ask that needs it.
class ITitles {
public:
    virtual ~ITitles() = default;

    [[nodiscard]] virtual const std::unordered_set<uint32_t>& getKnownTitleBits() const = 0;
    /// The bit worn, or -1 for none.
    [[nodiscard]] virtual int32_t getChosenTitleBit() const = 0;
    /// The title at a bit with the player's name in it, or empty.
    [[nodiscard]] virtual std::string getFormattedTitle(uint32_t bit) const = 0;
    /// A title by its CharTitles.dbc id (what a quest reward carries), with
    /// the player's name in it, or empty.
    [[nodiscard]] virtual std::string getFormattedTitleById(uint32_t id) const = 0;
    /// One player's title, decorated with that player's own name.
    [[nodiscard]] virtual std::string getFormattedTitleFor(uint32_t bit, const std::string& name) const = 0;
    /// CMSG_SET_TITLE: wear a title (bit >= 0) or none (-1).
    virtual void sendSetTitle(int32_t bit) = 0;
    /// Reads CharTitles.dbc now rather than on the first ask.
    virtual void loadTitles() = 0;
};

/// `titles`, or those of a client with no game: none known, none worn, no
/// names, and the request dropped - for a reader that would otherwise test for
/// it at every call.
[[nodiscard]] ITitles& orNoTitles(ITitles* titles);

/// What the protocol layer tells the titles.
class ITitlesFeed {
public:
    virtual ~ITitlesFeed() = default;

    /// SMSG_TITLE_EARNED.
    virtual void onEarned(uint32_t bit, bool lost) = 0;
    /// A server extension's known-title list.
    virtual void onKnownTitles(uint8_t operation, const std::vector<uint16_t>& bits) = 0;
    /// PLAYER_CHOSEN_TITLE.
    virtual void onChosen(int32_t bit) = 0;
    /// The player left the world - disconnected or back at the character
    /// list - so what the next character knows and wears is its own. Not at
    /// login: a realm's extension may send the known list before the
    /// character is chosen.
    virtual void leftWorld() = 0;
};

} // namespace wowee::title
