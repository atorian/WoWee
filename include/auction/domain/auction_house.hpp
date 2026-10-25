#pragma once

#include "auction/domain/events.hpp"
#include "auction/domain/records.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace wowee::auction {

/// The auction house the player is standing at: which auctioneer, the three
/// lists the server last sent, the last search, and how long until the next
/// search may go out.
class AuctionHouse {
public:
    std::vector<AuctionChange> opened(uint64_t auctioneerGuid);
    std::vector<AuctionChange> closed();
    /// A character is logging in: no auctioneer, list or search is theirs.
    void beginCharacter() { *this = AuctionHouse{}; }

    /// A search went out: it is remembered for the repeat after a purchase,
    /// and the next result fills the browse list.
    void searchSent(SearchQuery query);
    /// A request for the player's own auctions or bids went out.
    void listRequested(AuctionList list);
    /// A list arrived. `list` names it when the opcode does; otherwise it
    /// fills whichever list was last asked for.
    std::vector<AuctionChange> listReceived(ListResult result, std::optional<AuctionList> list);
    std::vector<AuctionChange> commandResult(uint32_t action, uint32_t error);
    void elapse(float seconds);
    /// An item's template arrived. A row carries only the item entry, so a
    /// list drawn before the answer shows "Item #41394" until it is told to
    /// redraw: each list waiting on this entry changed, while the house is open.
    std::vector<AuctionChange> itemInfoArrived(uint32_t itemEntry) const;

    /// Reorders a list in place, as the panel's column headers do: the server
    /// sends a list and the client sorts it.
    void reorder(AuctionList list, const std::function<bool(const Entry&, const Entry&)>& less);

    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] uint64_t auctioneer() const { return auctioneer_; }
    [[nodiscard]] const ListResult& list(AuctionList list) const;
    [[nodiscard]] const SearchQuery* lastSearch() const { return hasSearch_ ? &lastSearch_ : nullptr; }
    [[nodiscard]] float searchDelay() const { return searchDelay_; }
    [[nodiscard]] int activeTab() const { return activeTab_; }
    void selectTab(int tab) { activeTab_ = tab; }

private:
    ListResult& listRef(AuctionList list);

    bool open_ = false;
    uint64_t auctioneer_ = 0;
    int activeTab_ = 0;
    ListResult browse_;
    ListResult owned_;
    ListResult bids_;
    SearchQuery lastSearch_;
    bool hasSearch_ = false;
    AuctionList pending_ = AuctionList::Browse;
    float searchDelay_ = 0.0f;
};

} // namespace wowee::auction
