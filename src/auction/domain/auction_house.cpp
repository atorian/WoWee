#include "auction/domain/auction_house.hpp"

#include <algorithm>

namespace wowee::auction {

std::vector<AuctionChange> AuctionHouse::opened(uint64_t auctioneerGuid) {
    auctioneer_ = auctioneerGuid;
    open_ = true;
    activeTab_ = 0;
    return {AuctionHouseOpened{}};
}

std::vector<AuctionChange> AuctionHouse::closed() {
    open_ = false;
    auctioneer_ = 0;
    return {AuctionHouseClosed{}};
}

void AuctionHouse::searchSent(SearchQuery query) {
    lastSearch_ = std::move(query);
    hasSearch_ = true;
    pending_ = AuctionList::Browse;
    // Blocks the Search button until the answer comes back. The result
    // carries the server's own wait and replaces this; five seconds only
    // applies while a reply is outstanding, and on Classic and TBC, whose
    // list result ends before that field.
    searchDelay_ = 5.0f;
}

void AuctionHouse::listRequested(AuctionList list) {
    pending_ = list;
}

ListResult& AuctionHouse::listRef(AuctionList list) {
    switch (list) {
        case AuctionList::Owned: return owned_;
        case AuctionList::Bids:  return bids_;
        default:                 return browse_;
    }
}

const ListResult& AuctionHouse::list(AuctionList list) const {
    return const_cast<AuctionHouse*>(this)->listRef(list);
}

std::vector<AuctionChange> AuctionHouse::listReceived(ListResult result, std::optional<AuctionList> list) {
    if (list) pending_ = *list;
    // The server's own wait, in milliseconds; AzerothCore's is 300. Zero means
    // the field was not sent, and the client's floor stays.
    if (result.searchDelay > 0) searchDelay_ = static_cast<float>(result.searchDelay) / 1000.0f;
    listRef(pending_) = std::move(result);
    return {ListChanged{pending_}};
}

std::vector<AuctionChange> AuctionHouse::commandResult(uint32_t action, uint32_t error) {
    if (error != 0) return {ActionFailed{action, error}};
    return {ActionSucceeded{action}};
}

std::vector<AuctionChange> AuctionHouse::itemInfoArrived(uint32_t itemEntry) const {
    std::vector<AuctionChange> changes;
    if (!open_) return changes;
    for (AuctionList which : {AuctionList::Browse, AuctionList::Owned, AuctionList::Bids}) {
        const auto& auctions = list(which).auctions;
        const bool waiting = std::any_of(auctions.begin(), auctions.end(),
                                         [itemEntry](const Entry& e) { return e.itemEntry == itemEntry; });
        if (waiting) changes.emplace_back(ListChanged{which});
    }
    return changes;
}

void AuctionHouse::elapse(float seconds) {
    if (searchDelay_ <= 0.0f) return;
    searchDelay_ = std::max(0.0f, searchDelay_ - seconds);
}

void AuctionHouse::reorder(AuctionList list, const std::function<bool(const Entry&, const Entry&)>& less) {
    auto& auctions = listRef(list).auctions;
    std::stable_sort(auctions.begin(), auctions.end(), less);
}

} // namespace wowee::auction
