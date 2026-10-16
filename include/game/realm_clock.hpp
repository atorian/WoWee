#pragma once

#include "game/packed_time.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace wowee::game {

/**
 * The realm's calendar time, in seconds since 1970, kept running between the
 * packets that say it.
 *
 * GameClock answers "what hour is it in the world"; this answers "what moment
 * is it on the realm", which every client logged into it agrees on to within
 * a second or so. That agreement is the point: anything drawn from it - the
 * wind is the first - comes out the same on two screens side by side without
 * the server knowing it exists.
 *
 * Two sources, a fine one and a coarse one:
 *
 *   * The server's Unix time, to the second: SMSG_QUERY_TIME_RESPONSE, which
 *     the client asks for at world entry, and SMSG_SERVERTIME where a server
 *     sends it. This is the clock's real source and always wins.
 *   * The packed date in SMSG_LOGIN_SETTIMESPEED and the game-time packets,
 *     to the minute. It is the realm's local time (the server packs it from
 *     localtime), so it is read here as though it were UTC - consistent
 *     between clients of one realm, a time zone away from the Unix time.
 *     Taken only until the Unix time arrives, a moment later at login; once
 *     that has, a packed date says nothing the clock does not know better.
 *
 * Kept running on a monotonic clock from the moment each was received, so a
 * frame hitch or a dropped update does not make it drift.
 */
class RealmClock {
public:
    using Clock = std::chrono::steady_clock;

    enum class Source : uint8_t {
        None,        ///< nothing said yet: no realm, or not yet in the world
        PackedDate,  ///< a packed date, to the minute, realm-local read as UTC
        UnixTime,    ///< the server's Unix time, to the second
    };

    /// The server says it is `unixSeconds` at `at`.
    void setUnixTime(uint32_t unixSeconds, Clock::time_point at);

    /// The server's packed date and time, received at `at`. Returns false when
    /// it was not taken: the Unix time is already known, or the date is not one
    /// (a zero packs to 1 January 2000, which is how "no date" arrives, and a
    /// field out of range is no date either). The caller says which.
    bool setPackedDate(const WowDate& date, Clock::time_point at);

    [[nodiscard]] Source source() const { return source_; }

    /// Seconds since 1970 on the realm at `at`; empty before anything was said.
    [[nodiscard]] std::optional<double> seconds(Clock::time_point at) const;

    /// The seconds since 1970 a packed date names, or empty when it names no
    /// date at all.
    [[nodiscard]] static std::optional<double> packedDateSeconds(const WowDate& date);

private:
    double anchorSeconds_ = 0.0;
    Clock::time_point anchorAt_{};
    Source source_ = Source::None;
};

}  // namespace wowee::game
