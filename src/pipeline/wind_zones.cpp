#include "pipeline/wind_zones.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

namespace wowee {
namespace pipeline {

namespace {

using json = nlohmann::json;

/// [a, b] with a <= b, both at least `least`.
std::optional<std::pair<float, float>> readRange(const json& v, float least) {
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) {
        return std::nullopt;
    }
    const float a = v[0].get<float>();
    const float b = v[1].get<float>();
    if (!(a >= least) || !(b >= a)) return std::nullopt;
    return std::make_pair(a, b);
}

/// A list of ids; empty and false when anything in it is not one.
bool readIds(const json& j, const char* key, std::vector<uint32_t>& out) {
    out.clear();
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return false;
    for (const auto& id : j[key]) {
        if (!id.is_number_unsigned()) return false;
        out.push_back(id.get<uint32_t>());
    }
    return true;
}

/// A regime name to weight object, as one weight per regime. Says why when it
/// will not do.
bool readWeights(const json& v, const std::vector<WindRegime>& regimes, std::vector<float>& out,
                 std::string& why) {
    if (!v.is_object()) {
        why = "\"weights\" is not an object of regime names";
        return false;
    }
    out.assign(regimes.size(), 0.0f);
    float total = 0.0f;
    for (const auto& [name, w] : v.items()) {
        const auto it = std::find_if(regimes.begin(), regimes.end(),
                                     [&](const WindRegime& r) { return r.name == name; });
        if (it == regimes.end()) {
            why = "\"weights\" names regime \"" + name + "\", which \"regimes\" does not have";
            return false;
        }
        if (!w.is_number() || !(w.get<float>() >= 0.0f)) {
            why = "the weight of \"" + name + "\" is not a number of zero or more";
            return false;
        }
        out[static_cast<size_t>(it - regimes.begin())] = w.get<float>();
        total += w.get<float>();
    }
    if (!(total > 0.0f)) {
        why = "\"weights\" are all zero, so no regime could ever be drawn";
        return false;
    }
    return true;
}

/// Reads what a row may say over the defaults it starts from. False, with
/// `why`, when any of it is malformed; the row is then not used at all.
bool readOverrides(const json& j, const std::vector<WindRegime>& regimes, WindZoneRow& row,
                   std::string& why) {
    if (j.contains("direction")) {
        if (!j["direction"].is_number()) {
            why = "\"direction\" is not a number of degrees";
            return false;
        }
        row.directionDegrees = j["direction"].get<float>();
    }
    if (j.contains("weights") && !readWeights(j["weights"], regimes, row.weights, why)) {
        return false;
    }
    if (j.contains("dwellSeconds")) {
        const auto dwell = readRange(j["dwellSeconds"], 1.0f);
        if (!dwell) {
            why = "\"dwellSeconds\" is not [shortest, longest] of a second or more";
            return false;
        }
        row.dwellMinSeconds = dwell->first;
        row.dwellMaxSeconds = dwell->second;
    }
    if (j.contains("gusts")) {
        const auto& g = j["gusts"];
        if (!g.is_object()) {
            why = "\"gusts\" is not an object";
            return false;
        }
        for (const auto& [key, field] : {std::pair{"perMinuteScale", &row.gustsPerMinuteScale},
                                          std::pair{"strengthScale", &row.gustStrengthScale}}) {
            if (!g.contains(key)) continue;
            if (!g[key].is_number() || !(g[key].get<float>() >= 0.0f)) {
                why = std::string("\"gusts\".\"") + key + "\" is not a number of zero or more";
                return false;
            }
            *field = g[key].get<float>();
        }
    }
    return true;
}

std::optional<WindRegime> readRegime(const std::string& name, const json& j, std::string& why) {
    if (!j.is_object()) {
        why = "is not an object";
        return std::nullopt;
    }
    WindRegime r;
    r.name = name;
    const auto speed = j.contains("speed") ? readRange(j["speed"], 0.0f) : std::nullopt;
    if (!speed) {
        why = "has no \"speed\": [slowest, fastest] in yards per second";
        return std::nullopt;
    }
    r.speedMin = speed->first;
    r.speedMax = speed->second;
    if (j.contains("gusts")) {
        const auto& g = j["gusts"];
        const bool ok = g.is_object() &&
            (!g.contains("perMinute") || (g["perMinute"].is_number() && g["perMinute"].get<float>() >= 0.0f)) &&
            (!g.contains("strength") || (g["strength"].is_number() && g["strength"].get<float>() >= 1.0f));
        if (!ok) {
            why = "has \"gusts\" that are not { \"perMinute\": zero or more, \"strength\": one or more }";
            return std::nullopt;
        }
        r.gustsPerMinute = g.value("perMinute", 0.0f);
        r.gustStrength = g.value("strength", 1.0f);
    }
    return r;
}

std::string rowLabel(const json& j, size_t index) {
    std::string label = "zones[" + std::to_string(index) + "]";
    if (j.is_object() && j.contains("name") && j["name"].is_string()) {
        label += " \"" + j["name"].get<std::string>() + "\"";
    }
    return label;
}

} // namespace

WindZoneTable WindZoneTable::fallback() {
    WindZoneTable t;
    WindRegime light;
    light.name = "light";
    light.speedMin = 1.0f;
    light.speedMax = 3.0f;
    light.gustsPerMinute = 1.0f;
    light.gustStrength = 1.3f;
    t.regimes_.push_back(light);
    t.default_.name = "default";
    t.default_.directionDegrees = 37.0f;
    t.default_.weights = {1.0f};
    t.default_.dwellMinSeconds = 120.0f;
    t.default_.dwellMaxSeconds = 400.0f;
    return t;
}

const WindZoneRow& WindZoneTable::rowFor(uint32_t areaId, uint32_t zoneId) const {
    // Subzone rows first: "the pass, not the zone around it".
    if (areaId != 0) {
        for (const auto& row : rows_) {
            if (std::find(row.areas.begin(), row.areas.end(), areaId) != row.areas.end()) return row;
        }
    }
    if (zoneId != 0) {
        for (const auto& row : rows_) {
            if (std::find(row.zones.begin(), row.zones.end(), zoneId) != row.zones.end()) return row;
        }
    }
    return default_;
}

std::optional<size_t> WindZoneTable::regimeIndex(std::string_view name) const {
    for (size_t i = 0; i < regimes_.size(); ++i) {
        if (regimes_[i].name == name) return i;
    }
    return std::nullopt;
}

WindWeatherRule WindZoneTable::weatherRule(WeatherKind kind) const {
    switch (kind) {
        case WeatherKind::Rain:
        case WeatherKind::BlackRain: return rain_;
        case WeatherKind::Snow:
        case WeatherKind::BlackSnow: return snow_;
        case WeatherKind::Thunderstorm:
        case WeatherKind::Sandstorm: return storm_;
        case WeatherKind::Clear:
        case WeatherKind::Fog: return {};
    }
    return {};
}

WindZoneTable parseWindZones(std::string_view jsonText, const std::string& origin) {
    // Comments allowed: this is an authored file, and a table of winds without
    // notes beside it is a table nobody can maintain.
    const auto j = json::parse(jsonText.begin(), jsonText.end(), nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        LOG_WARNING("Wind zones: ", origin, " is not a JSON object; the built-in light wind "
                    "toward 37 degrees blows everywhere instead");
        return WindZoneTable::fallback();
    }

    WindZoneTable table;
    const WindZoneTable fallback = WindZoneTable::fallback();

    // ---- Regimes -----------------------------------------------------------
    if (j.contains("regimes") && j["regimes"].is_object()) {
        for (const auto& [name, r] : j["regimes"].items()) {
            std::string why;
            if (auto regime = readRegime(name, r, why)) {
                table.regimes_.push_back(std::move(*regime));
            } else {
                LOG_WARNING("Wind zones: ", origin, ": regime \"", name, "\" ", why,
                            "; left out, and rows weighting it are left out with it");
            }
        }
    }
    if (table.regimes_.empty()) {
        LOG_WARNING("Wind zones: ", origin, " has no usable \"regimes\"; the built-in light wind "
                    "toward 37 degrees blows everywhere instead");
        return fallback;
    }

    // ---- Default -----------------------------------------------------------
    // What a row does not say comes from here, and what this does not say
    // comes from the built-in default - weighted to "light" when the file has
    // one, else to its first regime.
    WindZoneRow& def = table.default_;
    def = fallback.default_;
    def.weights.assign(table.regimes_.size(), 0.0f);
    def.weights[table.regimeIndex("light").value_or(0)] = 1.0f;
    if (!j.contains("default") || !j["default"].is_object()) {
        LOG_WARNING("Wind zones: ", origin, " has no \"default\" object; the default wind is "
                    "the built-in one: toward ", def.directionDegrees, " degrees, regime \"",
                    table.regimes_[table.regimeIndex("light").value_or(0)].name, "\"");
    } else {
        WindZoneRow parsed = def;
        std::string why;
        if (readOverrides(j["default"], table.regimes_, parsed, why)) {
            def = std::move(parsed);
        } else {
            LOG_WARNING("Wind zones: ", origin, ": \"default\": ", why,
                        "; the default wind is the built-in one: toward ", def.directionDegrees,
                        " degrees, regime \"",
                        table.regimes_[table.regimeIndex("light").value_or(0)].name, "\"");
        }
    }
    def.name = "default";
    def.zones.clear();
    def.areas.clear();

    // ---- Zone rows ---------------------------------------------------------
    if (j.contains("zones")) {
        if (!j["zones"].is_array()) {
            LOG_WARNING("Wind zones: ", origin, ": \"zones\" is not an array; every zone takes "
                        "the default wind");
        } else {
            size_t index = 0;
            for (const auto& r : j["zones"]) {
                const std::string label = rowLabel(r, index++);
                WindZoneRow row = def;
                row.name = (r.is_object() && r.contains("name") && r["name"].is_string())
                               ? r["name"].get<std::string>()
                               : label;
                std::string why;
                bool ok = r.is_object();
                if (!ok) why = "is not an object";
                if (ok && (!readIds(r, "zones", row.zones) || !readIds(r, "areas", row.areas))) {
                    ok = false;
                    why = "\"zones\" or \"areas\" is not a list of ids";
                }
                if (ok && row.zones.empty() && row.areas.empty()) {
                    ok = false;
                    why = "names no \"zones\" and no \"areas\", so it could never match";
                }
                if (ok && !readOverrides(r, table.regimes_, row, why)) ok = false;
                if (!ok) {
                    LOG_WARNING("Wind zones: ", origin, ": ", label, " ", why,
                                "; left out, and the places it names take the default wind");
                    continue;
                }
                table.rows_.push_back(std::move(row));
            }
        }
    }

    // ---- Weather -----------------------------------------------------------
    if (j.contains("weather")) {
        if (!j["weather"].is_object()) {
            LOG_WARNING("Wind zones: ", origin, ": \"weather\" is not an object; weather leaves "
                        "the wind alone");
        } else {
            for (const auto& [kind, rule] : j["weather"].items()) {
                WindWeatherRule* target = kind == "rain"  ? &table.rain_
                                        : kind == "snow"  ? &table.snow_
                                        : kind == "storm" ? &table.storm_
                                                          : nullptr;
                if (!target) {
                    LOG_WARNING("Wind zones: ", origin, ": weather \"", kind,
                                "\" is not rain, snow or storm; ignored");
                    continue;
                }
                for (const char* verb : {"favour", "force"}) {
                    if (!rule.is_object() || !rule.contains(verb)) continue;
                    const auto& name = rule[verb];
                    const auto idx = name.is_string() ? table.regimeIndex(name.get<std::string>())
                                                      : std::nullopt;
                    if (!idx) {
                        LOG_WARNING("Wind zones: ", origin, ": weather \"", kind, "\" ", verb,
                                    "s ", name.dump(), ", which is not a regime; ignored");
                        continue;
                    }
                    (std::string_view(verb) == "force" ? target->force : target->favour) = idx;
                }
            }
        }
    }
    return table;
}

WindZoneTable loadWindZonesFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        LOG_WARNING("Wind zones: cannot read ", path, "; the built-in light wind toward 37 "
                    "degrees blows everywhere instead");
        return WindZoneTable::fallback();
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    WindZoneTable table = parseWindZones(text, path);
    LOG_INFO("Wind zones: ", path, ": ", table.regimes().size(), " regimes, ",
             table.zoneRows().size(), " zone rows");
    return table;
}

} // namespace pipeline
} // namespace wowee
