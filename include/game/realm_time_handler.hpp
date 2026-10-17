#pragma once

#include "core/event_bus.hpp"
#include "game/game_clock.hpp"
#include "game/opcode_table.hpp"
#include "game/realm_clock.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wowee::network { class Packet; }

namespace wowee::game {

/// What time it is on the realm: the hour of the world's day, which the sky
/// follows, and the calendar moment, which every client of the realm agrees
/// on (RealmClock).
///
/// The server says the first with SMSG_LOGIN_SETTIMESPEED at login and
/// SMSG_GAMETIME_SET/UPDATE and SMSG_GAMESPEED_SET if it changes it, all of
/// which carry a packed date as well; the second, to the second, with
/// SMSG_SERVERTIME where a server sends it and in the reply to the
/// CMSG_QUERY_TIME asked at world entry. That reply is SocialHandler's - it
/// also carries the daily reset - and is heard here as events::ServerTimeReceived.
///
/// Both clocks are kept running on `now`, the session's monotonic clock: real
/// time in the client, the test's own in a harness.
class RealmTimeHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;
    using Clock = std::chrono::steady_clock;

    RealmTimeHandler(core::EventBus& events, std::function<Clock::time_point()> now);

    void registerOpcodes(DispatchTable& table);

    // --- Queries -------------------------------------------------------------

    /// The world's hour, 0-24, or -1 before the server has said.
    [[nodiscard]] float gameHours() const { return gameClock_.hours(now_()); }

    /// Seconds since 1970 on the realm, or empty before the server has said
    /// anything that gives it. See RealmClock for how fine it is.
    [[nodiscard]] std::optional<double> realmSeconds() const { return realmClock_.seconds(now_()); }
    [[nodiscard]] RealmClock::Source realmClockSource() const { return realmClock_.source(); }

private:
    void handleLoginSetTimeSpeed(network::Packet& packet);
    void handleGameTime(network::Packet& packet);
    void handleGameSpeed(network::Packet& packet);
    void handleServerTime(network::Packet& packet);
    /// The calendar half of a packed time, for the realm clock.
    void takePackedDate(const WowDate& date, const char* from);
    void takeUnixTime(uint32_t unixSeconds, const char* from);

    std::function<Clock::time_point()> now_;
    GameClock gameClock_;
    RealmClock realmClock_;
    /// A packed date that was no date has been said, so it is said once.
    bool badPackedDateSaid_ = false;
    std::vector<core::EventBus::Subscription> subscriptions_;
};

}  // namespace wowee::game
