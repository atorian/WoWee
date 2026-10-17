// What the client's resource telemetry keeps and says: the ten minutes of
// history the performance HUD graphs, the frames each sample summarises, when
// the resource line is due, and the warnings a crash log should end with -
// once per crossing of a threshold, not once a sample.
#include <catch_amalgamated.hpp>

#include "core/resource_telemetry.hpp"

#include <optional>
#include <string>
#include <vector>

using wowee::core::ResourceAlert;
using wowee::core::ResourceSample;
using wowee::core::ResourceSeries;
using wowee::core::ResourceTelemetry;

namespace {

constexpr uint64_t kGiB = 1024ull * 1024 * 1024;

ResourceSample withThreads(double t, uint32_t threads) {
    ResourceSample s;
    s.timeSeconds = t;
    s.process.threadCount = threads;
    return s;
}

ResourceSample withCommit(double t, uint64_t used, uint64_t limit) {
    ResourceSample s;
    s.timeSeconds = t;
    s.process.systemCommitUsedBytes = used;
    s.process.systemCommitLimitBytes = limit;
    return s;
}

/// The alerts of one kind among those recorded, as crossed (true) / left (false).
std::vector<bool> alertsOf(const ResourceTelemetry::Recorded& r, ResourceAlert::Kind kind) {
    std::vector<bool> out;
    for (const auto& alert : r.alerts) {
        if (alert.kind == kind) out.push_back(alert.crossed);
    }
    return out;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("the history keeps the newest samples, oldest first", "[resources]") {
    ResourceTelemetry::Config config;
    config.historySamples = 600;
    ResourceTelemetry telemetry(config);
    for (uint32_t i = 0; i < 650; ++i) telemetry.record(withThreads(i, i));

    const auto threads = telemetry.series(ResourceSeries::Threads).measured();
    REQUIRE(threads.size() == 600);
    CHECK(threads.front() == 50.0f);
    CHECK(threads.back() == 649.0f);
}

TEST_CASE("a figure that was not measured is a gap in its graph, not a zero", "[resources]") {
    ResourceTelemetry telemetry(ResourceTelemetry::Config{});
    for (int i = 0; i < 5; ++i) {
        ResourceSample s;
        s.timeSeconds = i;
        // In the world for the first three seconds, then at the character screen.
        if (i < 3) s.latencyMs = 100 + i;
        telemetry.record(s);
    }
    const auto& latency = telemetry.series(ResourceSeries::LatencyMs);
    CHECK(latency.size() == 5);
    CHECK(latency.measured() == std::vector<float>{100.0f, 101.0f, 102.0f});
}

TEST_CASE("each sample summarises the frames since the one before", "[resources]") {
    ResourceTelemetry telemetry(ResourceTelemetry::Config{});
    for (int i = 0; i < 60; ++i) telemetry.noteFrame(1.0f / 60.0f);
    telemetry.record(ResourceSample{});
    CHECK(telemetry.latest()->frames.frames == 60);
    CHECK(telemetry.latest()->frames.averageFps == Catch::Approx(60.0f));

    for (int i = 0; i < 30; ++i) telemetry.noteFrame(1.0f / 30.0f);
    ResourceSample next;
    next.timeSeconds = 1.0;
    telemetry.record(next);
    CHECK(telemetry.latest()->frames.frames == 30);
    CHECK(telemetry.latest()->frames.averageFps == Catch::Approx(30.0f));
    CHECK(telemetry.series(ResourceSeries::FrameRate).measured().size() == 2);
}

TEST_CASE("the resource line is due once an interval and covers the frames since the last",
          "[resources]") {
    ResourceTelemetry::Config config;
    config.logIntervalSeconds = 10.0;
    ResourceTelemetry telemetry(config);

    std::vector<std::string> lines;
    for (int second = 0; second <= 20; ++second) {
        // 99 quick frames and one hitch of a tenth of a second.
        for (int f = 0; f < 99; ++f) telemetry.noteFrame(0.016f);
        telemetry.noteFrame(0.1f);
        ResourceSample s;
        s.timeSeconds = second;
        s.latencyMs = 87;
        s.process.workingSetBytes = 3 * kGiB;
        s.process.threadCount = 212;
        s.caches.push_back({"M2 textures", {3, kGiB + kGiB / 2, 2 * kGiB}});
        s.counts.push_back({"normal-map jobs", 7});
        if (auto line = telemetry.record(s).logLine) lines.push_back(*line);
    }

    // At ten and at twenty seconds, not before.
    REQUIRE(lines.size() == 2);
    const std::string& line = lines.front();
    // The hitch is the slowest frame; 99% of frames were quicker than it.
    CHECK(contains(line, "10.0 min"));
    CHECK(contains(line, "p99 16.0 ms"));
    CHECK(contains(line, "latency 87 ms"));
    CHECK(contains(line, "working set 3.00 GB"));
    CHECK(contains(line, "private n/a"));
    CHECK(contains(line, "threads 212"));
    CHECK(contains(line, "M2 textures 1.50 GB of 2.00 GB (3)"));
    CHECK(contains(line, "normal-map jobs 7"));
}

TEST_CASE("too many threads is said once when crossed and once when left", "[resources]") {
    ResourceTelemetry::Config config;
    config.thresholds.maxThreads = 512;
    ResourceTelemetry telemetry(config);
    using Kind = ResourceAlert::Kind;

    CHECK(alertsOf(telemetry.record(withThreads(0, 500)), Kind::Threads).empty());
    const auto crossed = telemetry.record(withThreads(1, 600));
    REQUIRE(alertsOf(crossed, Kind::Threads) == std::vector<bool>{true});
    CHECK(contains(crossed.alerts.front().message, "600 threads"));
    // The line goes with the warning, so the log says what else was held.
    CHECK(contains(crossed.alerts.front().message, "resources:"));
    // Still over, and higher: nothing more.
    CHECK(alertsOf(telemetry.record(withThreads(2, 700)), Kind::Threads).empty());
    // Just under the limit is not back yet; well under is.
    CHECK(alertsOf(telemetry.record(withThreads(3, 500)), Kind::Threads).empty());
    CHECK(alertsOf(telemetry.record(withThreads(4, 400)), Kind::Threads) == std::vector<bool>{false});
    // A second climb is a second crossing.
    CHECK(alertsOf(telemetry.record(withThreads(5, 900)), Kind::Threads) == std::vector<bool>{true});
}

TEST_CASE("the system running out of commit is said once a crossing", "[resources]") {
    ResourceTelemetry::Config config;
    config.thresholds.minSystemCommitFreeBytes = 2 * kGiB;
    ResourceTelemetry telemetry(config);
    using Kind = ResourceAlert::Kind;
    const uint64_t limit = 57 * kGiB;

    CHECK(alertsOf(telemetry.record(withCommit(0, 54 * kGiB, limit)), Kind::SystemCommit).empty());
    const auto low = telemetry.record(withCommit(1, limit - kGiB, limit));
    REQUIRE(alertsOf(low, Kind::SystemCommit) == std::vector<bool>{true});
    CHECK(contains(low.alerts.front().message, "1.00 GB of commit left"));
    CHECK(alertsOf(telemetry.record(withCommit(2, limit - kGiB / 2, limit)), Kind::SystemCommit).empty());
    CHECK(alertsOf(telemetry.record(withCommit(3, 50 * kGiB, limit)), Kind::SystemCommit) ==
          std::vector<bool>{false});
}

TEST_CASE("the client committing over half the machine's memory is said once", "[resources]") {
    ResourceTelemetry::Config config;
    config.thresholds.maxPrivateShareOfPhysical = 0.5;
    ResourceTelemetry telemetry(config);
    using Kind = ResourceAlert::Kind;
    const auto sample = [](double t, uint64_t privateBytes) {
        ResourceSample s;
        s.timeSeconds = t;
        s.process.privateBytes = privateBytes;
        s.process.totalPhysicalBytes = 32 * kGiB;
        return s;
    };

    CHECK(alertsOf(telemetry.record(sample(0, 10 * kGiB)), Kind::PrivateCommit).empty());
    CHECK(alertsOf(telemetry.record(sample(1, 17 * kGiB)), Kind::PrivateCommit) == std::vector<bool>{true});
    CHECK(alertsOf(telemetry.record(sample(2, 20 * kGiB)), Kind::PrivateCommit).empty());
}

namespace {

ResourceSample withVideoMemory(double t, uint64_t usage, uint64_t budget) {
    ResourceSample s;
    s.timeSeconds = t;
    s.videoMemory = ResourceSample::VideoMemory{.budgetBytes = budget, .usageBytes = usage, .parts = {}};
    return s;
}

} // namespace

TEST_CASE("video memory is graphed and said in the resource line with what it holds", "[resources]") {
    ResourceTelemetry::Config config;
    config.logIntervalSeconds = 10.0;
    ResourceTelemetry telemetry(config);

    std::optional<std::string> line;
    for (int second = 0; second <= 10; ++second) {
        auto s = withVideoMemory(second, 3 * kGiB + kGiB / 2, 10 * kGiB);
        s.videoMemory->parts = {{"render targets", 400ull * 1024 * 1024},
                                {"normal maps", 2 * kGiB}};
        if (auto l = telemetry.record(s).logLine) line = l;
    }

    REQUIRE(line);
    CHECK(contains(*line, "video memory 3.50 GB of 10.00 GB budget"));
    CHECK(contains(*line, "render targets 400 MB"));
    CHECK(contains(*line, "normal maps 2.00 GB"));
    const auto graph = telemetry.series(ResourceSeries::VideoMemoryMB).measured();
    REQUIRE(graph.size() == 11);
    CHECK(graph.back() == Catch::Approx(3.5f * 1024.0f));
}

TEST_CASE("video memory close to the driver's budget is said once a crossing", "[resources]") {
    ResourceTelemetry::Config config;
    config.thresholds.maxVideoMemoryShareOfBudget = 0.95;
    ResourceTelemetry telemetry(config);
    using Kind = ResourceAlert::Kind;
    const uint64_t budget = 10 * kGiB;

    CHECK(alertsOf(telemetry.record(withVideoMemory(0, 8 * kGiB, budget)), Kind::VideoMemory).empty());
    const auto full = telemetry.record(withVideoMemory(1, budget - kGiB / 4, budget));
    REQUIRE(alertsOf(full, Kind::VideoMemory) == std::vector<bool>{true});
    CHECK(contains(full.alerts.front().message, "of the 10.00 GB"));
    CHECK(alertsOf(telemetry.record(withVideoMemory(2, budget, budget)), Kind::VideoMemory).empty());
    CHECK(alertsOf(telemetry.record(withVideoMemory(3, 6 * kGiB, budget)), Kind::VideoMemory) ==
          std::vector<bool>{false});
}
