#pragma once

#include <string>
#include <variant>

namespace wowee::trade {

// What happened to a trade. Trade returns these; the service publishes them,
// and the chat log and the addon layer subscribe.

/// Another player asks to trade (TRADE_REQUEST).
struct TradeRequested { std::string peerName; };
struct TradeShown {};
/// The other side pressed accept.
struct PartnerAccepted {};
/// Who has accepted, for TRADE_ACCEPT_UPDATE, which reads both.
struct AcceptStateChanged { bool self = false; bool partner = false; };

enum class Ending { Cancelled, Declined, WindowClosed, Completed };
struct TradeEnded { Ending how = Ending::Cancelled; };

enum class Refusal {
    Busy, NoTarget, TooFar, WrongFaction, Ignored, LoggingOut,
    YouStunned, TargetStunned, YouDead, TargetDead, NotOnTapList,
};
struct TradeRefused { Refusal why = Refusal::Busy; };

/// Either side's items changed (TRADE_UPDATE).
struct OffersChanged {};
/// The gold one side has put down changed; each side has its own money frame.
struct OfferedMoneyChanged { bool own = false; };

using TradeChange = std::variant<TradeRequested, TradeShown, PartnerAccepted, AcceptStateChanged,
                                 TradeEnded, TradeRefused, OffersChanged, OfferedMoneyChanged>;

// What became of a request the player made, published by the service.

/// A trade asked of the player under the target.
struct TradeProposed {};
/// initiateTrade without anyone to trade with.
struct NoTradeTarget {};
/// A request turned down because the player blocks trades. Said in chat, as a
/// request swallowed silently reads as the other player being ignored.
struct TradeRequestBlocked { std::string peerName; };

} // namespace wowee::trade
