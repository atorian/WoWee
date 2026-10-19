#include "core/benchmark_run.hpp"

#include "core/local_time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wowee::core {

namespace {

constexpr float kPi = 3.14159265358979f;

/// A lap round `centre` at `radius`, `eyeHeight` above it, looking at the
/// point `lookHeight` above it: the whole of a city's middle passes through
/// the view once, from every side.
BenchmarkRoute orbit(std::string name, uint32_t mapId, uint32_t zoneId, glm::vec3 centre, float hour,
                     float radius, float eyeHeight, float lookHeight) {
    constexpr int kWaypoints = 12;
    BenchmarkRoute route{std::move(name), mapId, zoneId, centre, hour, {}};
    for (int i = 0; i < kWaypoints; ++i) {
        const float angle = 2.0f * kPi * static_cast<float>(i) / kWaypoints;
        const glm::vec3 eye = centre + glm::vec3(radius * std::cos(angle), radius * std::sin(angle), eyeHeight);
        route.loop.push_back({eye, centre + glm::vec3(0.0f, 0.0f, lookHeight)});
    }
    return route;
}

/// A flight from a character standing at `path`'s first point, `groundZ`,
/// along `path` at `altitude`, looking ahead at the next point
/// `lookDrop` lower, and back from the last point to the first: the terrain
/// streams in ahead of it and out behind it the whole way.
BenchmarkRoute crossing(std::string name, uint32_t mapId, uint32_t zoneId, float groundZ, float hour,
                        const std::vector<glm::vec2>& path, float altitude, float lookDrop) {
    const glm::vec2 start = path.front();
    BenchmarkRoute route{std::move(name), mapId, zoneId, glm::vec3(start, groundZ), hour, {}};
    for (size_t i = 0; i < path.size(); ++i) {
        const glm::vec2 next = path[(i + 1) % path.size()];
        route.loop.push_back({glm::vec3(path[i], altitude), glm::vec3(next, altitude - lookDrop)});
    }
    return route;
}

/// Uniform Catmull-Rom through p1 and p2.
glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
                     float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3);
}

std::string ms(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

} // namespace

const std::vector<BenchmarkRoute>& benchmarkRoutes() {
    // The cities a frame is slowest in. Positions as .gps gives them, which
    // are also the renderer's: the two swaps between them cancel.
    static const std::vector<BenchmarkRoute> routes = {
        // Dalaran's centre at noon, above the streets and below the spires.
        orbit("dalaran", 571, 4395, {5804.0f, 624.0f, 647.0f}, 12.0f, 100.0f, 35.0f, 5.0f),
        // Stormwind's trade district in the afternoon.
        orbit("stormwind", 0, 1519, {-8833.0f, 628.0f, 94.0f}, 16.5f, 90.0f, 30.0f, 4.0f),
        // Orgrimmar's Valley of Strength in the morning.
        orbit("orgrimmar", 1, 1637, {1676.0f, -4315.0f, 61.0f}, 9.0f, 70.0f, 30.0f, 4.0f),
        // Out of Stormwind over Goldshire, Sentinel Hill and Moonbrook, down
        // Stranglethorn's coast past Grom'gol to Booty Bay, and back north
        // over Darkshire: about 14,500 yards, twice the load radius away
        // from the start, so tiles stream in ahead and out behind the whole
        // way. Farms, inns, towers, a port - the hitches of finishing and
        // unloading tiles, which the city laps never see once settled.
        crossing("bootybay", 0, 12, 94.0f, 13.0f,
                 {{-8833.0f, 628.0f}, {-9460.0f, 62.0f}, {-10600.0f, 1040.0f}, {-11000.0f, 1500.0f},
                  {-11800.0f, 1000.0f}, {-12400.0f, 200.0f}, {-14300.0f, 450.0f}, {-12600.0f, -400.0f},
                  {-10560.0f, -1150.0f}, {-9500.0f, -300.0f}},
                 260.0f, 160.0f),
    };
    return routes;
}

const BenchmarkRoute* findBenchmarkRoute(std::string_view name) {
    for (const auto& route : benchmarkRoutes()) {
        if (route.name == name) return &route;
    }
    return nullptr;
}

BenchmarkCamera cameraAlong(const BenchmarkRoute& route, float fraction) {
    BenchmarkCamera out;
    const auto& loop = route.loop;
    if (loop.empty()) return out;
    const int n = static_cast<int>(loop.size());
    const float along = std::clamp(fraction, 0.0f, 1.0f) * static_cast<float>(n);
    const int segment = std::min(static_cast<int>(along), n - 1);
    const float t = along - static_cast<float>(segment);
    const auto at = [&](int i) -> const BenchmarkRoute::Waypoint& { return loop[((i % n) + n) % n]; };
    const glm::vec3 eye = catmullRom(at(segment - 1).eye, at(segment).eye, at(segment + 1).eye,
                                     at(segment + 2).eye, t);
    const glm::vec3 target = catmullRom(at(segment - 1).lookAt, at(segment).lookAt, at(segment + 1).lookAt,
                                        at(segment + 2).lookAt, t);
    const glm::vec3 d = target - eye;
    out.position = eye;
    out.yawDegrees = std::atan2(d.y, d.x) * 180.0f / kPi;
    out.pitchDegrees = std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * 180.0f / kPi;
    return out;
}

BenchmarkRun::BenchmarkRun(BenchmarkRoute route, Timing timing)
    : route_(std::move(route)), timing_(timing) {}

void BenchmarkRun::frameEnded(float frameSeconds, bool worldInView, bool streaming) {
    switch (phase_) {
    case Phase::WaitingForWorld:
        if (worldInView) {
            phase_ = Phase::Settling;
            settled_ = 0.0f;
        }
        break;
    case Phase::Settling:
        if (!worldInView) {
            phase_ = Phase::WaitingForWorld;
            break;
        }
        settled_ += frameSeconds;
        if ((settled_ >= timing_.settleSeconds && !streaming) || settled_ >= timing_.maxSettleSeconds) {
            phase_ = Phase::Flying;
            streamingAtTakeoff_ = streaming;
            flown_ = 0.0f;
            budget_.reset();
        }
        break;
    case Phase::Flying:
        flown_ += frameSeconds;
        if (flown_ >= timing_.flySeconds) phase_ = Phase::Finished;
        break;
    case Phase::Finished:
        break;
    }
}

std::optional<BenchmarkCamera> BenchmarkRun::camera() const {
    if (phase_ != Phase::Flying) return std::nullopt;
    const float fraction = timing_.flySeconds > 0.0f ? flown_ / timing_.flySeconds : 1.0f;
    return cameraAlong(route_, fraction);
}

void BenchmarkRun::noteSceneCount(std::string_view name, uint64_t value) {
    auto it = std::find_if(scene_.begin(), scene_.end(), [&](const SceneCount& c) { return c.name == name; });
    if (it == scene_.end()) {
        scene_.push_back({std::string(name), 0.0, 0, 0});
        it = scene_.end() - 1;
    }
    it->total += static_cast<double>(value);
    it->most = std::max(it->most, value);
    ++it->samples;
}

std::vector<std::string> BenchmarkRun::report(const std::vector<std::string>& setup) const {
    std::vector<std::string> out;
    {
        std::ostringstream head;
        head << "WoWee benchmark: route " << route_.name << " (map " << route_.mapId << ", zone "
             << route_.zoneId << "), hour " << route_.hour << ", one lap of " << route_.loop.size()
             << " waypoints in " << timing_.flySeconds << " s";
        out.push_back(head.str());
    }
    for (const auto& line : setup) out.push_back(line);
    {
        std::ostringstream settle;
        settle << "settled " << ms(settled_) << " s before the flight";
        if (streamingAtTakeoff_) {
            settle << " - terrain was still streaming in when it began (the "
                   << timing_.maxSettleSeconds << " s limit), so its first frames carry that work";
        }
        out.push_back(settle.str());
    }
    out.push_back(summary());
    for (auto& line : budget_.report("flight over " + ms(flown_) + "s", flown_ * 1000.0)) {
        out.push_back(std::move(line));
    }
    if (!scene_.empty()) {
        out.push_back("scene during the flight (average, most):");
        for (const auto& count : scene_) {
            std::ostringstream line;
            line << "  " << count.name << ": "
                 << static_cast<uint64_t>(std::llround(count.samples ? count.total / count.samples : 0.0))
                 << ", " << count.most;
            out.push_back(line.str());
        }
    }
    return out;
}

std::string BenchmarkRun::summary() const {
    const TimeDistribution frames = budget_.frameTimes();
    std::ostringstream line;
    line << "summary: route " << route_.name << ", " << frames.count << " frames, "
         << ms(flown_ > 0.0f ? frames.count / flown_ : 0.0) << " fps, frame p50 " << ms(frames.p50Ms)
         << " / p95 " << ms(frames.p95Ms) << " / p99 " << ms(frames.p99Ms) << " / max "
         << ms(frames.maxMs) << " ms";
    for (const float limit : kHitchLimitsMs) {
        const uint32_t over = budget_.framesOver(limit);
        char text[16];
        std::snprintf(text, sizeof(text), "%.1f", limit);
        line << ", over " << text << "ms: " << over << " ("
             << ms(frames.count ? 100.0 * over / frames.count : 0.0) << "%)";
    }
    return line.str();
}

std::optional<std::filesystem::path> writeBenchmarkReport(const std::filesystem::path& dir,
                                                         std::string_view route,
                                                         const std::vector<std::string>& lines) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::tm now = localTime(std::time(nullptr));
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &now);
    const std::filesystem::path path = dir / ("benchmark-" + std::string(route) + "-" + stamp + ".txt");
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file) return std::nullopt;
    for (const auto& line : lines) file << line << '\n';
    file.close();
    if (!file) return std::nullopt;
    return path;
}

} // namespace wowee::core
