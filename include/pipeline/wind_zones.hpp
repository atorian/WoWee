#pragma once

#include "pipeline/weather_kind.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Per-zone wind, authored in assets/wind_zones.json.
//
// The wind is a deterministic function of the realm's clock and the zone
// (WindField), so every client of a realm sees the same weather in the trees
// without the server knowing it exists. This file is what that function is
// made of:
//
//   "regimes"  named kinds of wind - calm, light, strong, storm - each a range
//              of speeds in yards per second and how often it gusts and how
//              hard;
//   "default"  the wind everywhere no zone row names: the direction it blows
//              toward, how much of the time each regime holds, and how long a
//              regime holds before the next is drawn ("dwellSeconds");
//   "zones"    rows that inherit everything from "default" and override what
//              they name. A row's "areas" match the subzone under the player
//              and win over every row's "zones", which match the zone it
//              belongs to; among equals the earlier row wins;
//   "weather"  what the realm's weather does to it: "favour" a regime (it is
//              drawn at least as often as all the others together) or
//              "force" one (it is the only one drawn).
//
// Directions are degrees in the renderer's ground plane - 0 toward +x (the
// game's west), 90 toward +y (its north) - the way the wind blows toward.
//
// A missing file or a row that does not parse is said once at warning level,
// naming the path and the row and what is used instead; nothing of it fails
// the client.

namespace wowee {
namespace pipeline {

/// A kind of wind.
struct WindRegime {
    std::string name;
    float speedMin = 0.0f;  ///< yards per second
    float speedMax = 0.0f;
    float gustsPerMinute = 0.0f;
    float gustStrength = 1.0f;  ///< a gust's peak speed as a multiple of the wind's
};

/// The wind of one place, with everything the file did not say for it filled
/// in from "default".
struct WindZoneRow {
    std::string name;
    std::vector<uint32_t> zones;  ///< matched against the resolved zone id
    std::vector<uint32_t> areas;  ///< matched against the subzone's own id; wins
    float directionDegrees = 0.0f;
    /// How much of the time each regime holds, one weight per entry of
    /// WindZoneTable::regimes, in that order. Never all zero.
    std::vector<float> weights;
    float dwellMinSeconds = 120.0f;
    float dwellMaxSeconds = 400.0f;
    float gustsPerMinuteScale = 1.0f;
    float gustStrengthScale = 1.0f;
};

/// What one kind of weather does to the wind: a regime index to favour or to
/// force, or neither.
struct WindWeatherRule {
    std::optional<size_t> favour;
    std::optional<size_t> force;
};

class WindZoneTable {
public:
    /// The table used when there is no file: one regime, "light", at 1-3
    /// yd/s with a gust a minute, blowing toward 37 degrees. The values the
    /// shipped file's default has, so a missing file changes nothing but the
    /// warning that says it is missing.
    [[nodiscard]] static WindZoneTable fallback();

    /// The row for this ground: a row naming the subzone, else one naming its
    /// zone, else the default.
    [[nodiscard]] const WindZoneRow& rowFor(uint32_t areaId, uint32_t zoneId) const;
    [[nodiscard]] const WindZoneRow& defaultRow() const { return default_; }
    [[nodiscard]] const std::vector<WindRegime>& regimes() const { return regimes_; }
    [[nodiscard]] const std::vector<WindZoneRow>& zoneRows() const { return rows_; }
    /// The index of a regime by name, or empty.
    [[nodiscard]] std::optional<size_t> regimeIndex(std::string_view name) const;
    /// What the realm's weather does: the file's "rain" for rain and black
    /// rain, "snow" for snow and black snow, "storm" for thunderstorms and
    /// sandstorms. Clear skies and fog, or a kind the file says nothing about,
    /// do nothing.
    [[nodiscard]] WindWeatherRule weatherRule(WeatherKind kind) const;

private:
    friend WindZoneTable parseWindZones(std::string_view jsonText, const std::string& origin);

    std::vector<WindRegime> regimes_;
    WindZoneRow default_;
    std::vector<WindZoneRow> rows_;
    WindWeatherRule rain_, snow_, storm_;
};

/// Parse the table from JSON text. Comments (// and /* */) are allowed.
/// `origin` names where the text came from in the log lines. Text that does
/// not parse, or that has no usable regime, gives WindZoneTable::fallback();
/// a row or a regime that is malformed is left out and said; a default that is
/// malformed is replaced by the fallback's. Each is a warning.
[[nodiscard]] WindZoneTable parseWindZones(std::string_view jsonText, const std::string& origin);

/// Read and parse a file. A file that is not there is a warning, and gives
/// WindZoneTable::fallback().
[[nodiscard]] WindZoneTable loadWindZonesFile(const std::string& path);

} // namespace pipeline
} // namespace wowee
