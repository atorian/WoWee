#pragma once

#include <array>
#include <cstdint>

namespace wowee::trade {

/// Seven slots a side: 0-5 change hands, and the seventh holds an item of
/// one's own that the other player may enchant or craft on without taking it.
inline constexpr int kSlotCount = 7;
inline constexpr int kNonTradedSlot = 6;

struct TradeSlot {
    uint32_t itemId = 0;
    uint32_t displayId = 0;
    uint32_t stackCount = 0;
    uint64_t itemGuid = 0;
    [[nodiscard]] bool occupied() const { return itemId != 0; }
};

using TradeSlots = std::array<TradeSlot, kSlotCount>;

/// One slot of an offer as SMSG_TRADE_STATUS_EXTENDED describes it.
struct OfferedSlot {
    uint8_t index = 0;
    uint32_t itemId = 0;
    uint32_t displayId = 0;
    uint32_t stackCount = 0;
};

/// SMSG_TRADE_STATUS, in AzerothCore's TradeStatus numbering.
enum class StatusCode : uint32_t {
    Busy = 0,
    BeginTrade = 1,
    OpenWindow = 2,
    Canceled = 3,
    Accepted = 4,
    Busy2 = 5,
    NoTarget = 6,
    BackToTrade = 7,
    Complete = 8,
    Rejected = 9,
    TooFar = 10,
    WrongFaction = 11,
    CloseWindow = 12,
    IgnoreYou = 14,
    YouStunned = 15,
    TargetStunned = 16,
    YouDead = 17,
    TargetDead = 18,
    YouLogout = 19,
    TargetLogout = 20,
    NotOnTapList = 23,
};

} // namespace wowee::trade
