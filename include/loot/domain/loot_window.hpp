#pragma once

#include "loot/domain/events.hpp"
#include "loot/domain/records.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace wowee::loot {

/// The loot window and what it remembers about each source looted: what the
/// source held when it opened, whether its money has been announced, whether
/// its items have been stored automatically.
class LootWindow {
public:
    /// A loot request went out; the items shown belong to whatever answers.
    void targetLooted() { contents_.items.clear(); }
    std::vector<LootChange> opened(LootContents contents);
    /// The player closed the window. Nothing when it is not open.
    std::vector<LootChange> closed();
    /// The server released the source.
    std::vector<LootChange> released();
    std::vector<LootChange> slotRemoved(uint8_t slot);
    /// The coin is gone, because the server has said so. Marked rather than
    /// dropped: it is display slot one, and dropping it would move every item
    /// button onto the item after its own.
    std::vector<LootChange> moneyCleared();
    std::vector<LootChange> moneyNotified(uint32_t amount);
    /// The money was asked for as the window opened. Should no notify come,
    /// it is announced after a moment anyway, unless it was just announced.
    void moneyRequested();
    std::vector<LootChange> timePassed(float seconds);
    /// Whether this source's items are still to be stored automatically; true
    /// once per source.
    bool claimAutoStore();
    std::vector<LootChange> holdForBind(uint8_t slot);
    /// The item request went out; nothing is held back any more.
    void itemRequested() { pendingBind_.reset(); }
    std::optional<uint8_t> takePendingBind();
    std::vector<LootChange> masterCandidatesReceived(std::vector<uint64_t> candidates);
    void setAutoLoot(bool enabled) { autoLoot_ = enabled; }
    /// A character is logging in: no window, source or money of the last
    /// one's is theirs. Autoloot is the player's setting and stays.
    void beginCharacter() {
        const bool autoLoot = autoLoot_;
        *this = LootWindow{};
        autoLoot_ = autoLoot;
    }
    /// An item's template arrived. The window drew its rows before the
    /// queries answered, so the rows naming it are redrawn.
    std::vector<LootChange> itemInfoArrived(uint32_t itemId) const;

    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] bool autoLoot() const { return autoLoot_; }
    [[nodiscard]] const LootContents& contents() const { return contents_; }
    /// The first item in a server slot, or null.
    [[nodiscard]] const LootItem* item(uint8_t slot) const;
    [[nodiscard]] const std::vector<uint64_t>& masterCandidates() const { return masterCandidates_; }
    /// Open, with nothing left in it.
    [[nodiscard]] bool emptied() const { return open_ && contents_.nothingLeft(); }
    /// Whether a source held items when it was last opened.
    [[nodiscard]] bool heldItems(uint64_t sourceGuid) const;

private:
    struct Source {
        LootContents data;
        bool moneyTaken = false;
        bool itemsAutoStored = false;
    };

    std::vector<LootChange> announce(uint64_t sourceGuid, uint32_t amount);

    bool open_ = false;
    bool autoLoot_ = false;
    LootContents contents_;
    std::vector<uint64_t> masterCandidates_;
    std::optional<uint8_t> pendingBind_;
    std::unordered_map<uint64_t, Source> sources_;
    /// The announcement a notify would have made, for a server that sends none.
    uint64_t pendingMoneyGuid_ = 0;
    uint32_t pendingMoneyAmount_ = 0;
    float pendingMoneyTimer_ = 0.0f;
    /// Sources whose money was announced a moment ago, which a reopened
    /// window does not announce again.
    std::unordered_map<uint64_t, float> announceCooldowns_;
};

} // namespace wowee::loot
