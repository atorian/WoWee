#include "core/resource_telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace wowee::core {

namespace {

constexpr double kMiB = 1024.0 * 1024.0;
constexpr float kNotMeasured = std::numeric_limits<float>::quiet_NaN();

template <typename T>
float orNotMeasured(const std::optional<T>& value, double scale = 1.0) {
    return value ? static_cast<float>(static_cast<double>(*value) / scale) : kNotMeasured;
}

std::string bytesOrNone(const std::optional<uint64_t>& bytes) {
    return bytes ? formatBytes(*bytes) : std::string("n/a");
}

} // namespace

std::string formatBytes(uint64_t bytes) {
    char text[32];
    const double value = static_cast<double>(bytes);
    if (value >= 1024.0 * kMiB) {
        std::snprintf(text, sizeof(text), "%.2f GB", value / (1024.0 * kMiB));
    } else if (value >= kMiB) {
        std::snprintf(text, sizeof(text), "%.0f MB", value / kMiB);
    } else {
        std::snprintf(text, sizeof(text), "%.0f KB", value / 1024.0);
    }
    return text;
}

// ---- SampleRing -------------------------------------------------------------

SampleRing::SampleRing(size_t capacity) : values_(std::max<size_t>(capacity, 1), 0.0f) {}

void SampleRing::push(float value) {
    values_[next_] = value;
    next_ = (next_ + 1) % values_.size();
    count_ = std::min(count_ + 1, values_.size());
}

float SampleRing::at(size_t i) const {
    const size_t oldest = (next_ + values_.size() - count_) % values_.size();
    return values_[(oldest + i) % values_.size()];
}

std::vector<float> SampleRing::measured() const {
    std::vector<float> out;
    out.reserve(count_);
    for (size_t i = 0; i < count_; ++i) {
        const float value = at(i);
        if (!std::isnan(value)) out.push_back(value);
    }
    return out;
}

// ---- Frames -----------------------------------------------------------------

FrameSummary ResourceTelemetry::FrameWindow::summary() const {
    FrameSummary out;
    if (seconds.empty()) return out;
    double total = 0.0;
    float slowest = 0.0f;
    for (const float s : seconds) {
        total += s;
        slowest = std::max(slowest, s);
    }
    std::vector<float> sorted = seconds;
    // The frame at the 99th percentile, counting up from the fastest.
    const size_t rank = std::min(sorted.size() - 1,
                                 static_cast<size_t>(std::ceil(0.99 * sorted.size())) - 1);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank), sorted.end());
    out.frames = static_cast<uint32_t>(seconds.size());
    out.averageFps = total > 0.0 ? static_cast<float>(seconds.size() / total) : 0.0f;
    out.minimumFps = slowest > 0.0f ? 1.0f / slowest : 0.0f;
    out.p99FrameMs = sorted[rank] * 1000.0f;
    return out;
}

// ---- Thresholds -------------------------------------------------------------

std::optional<bool> ResourceTelemetry::Watch::update(double value, double limit, double margin) {
    if (!over && value > limit) {
        over = true;
        return true;
    }
    if (over && value < limit - margin) {
        over = false;
        return false;
    }
    return std::nullopt;
}

void ResourceTelemetry::checkThresholds(const ResourceSample& sample, const std::string& line,
                                        std::vector<ResourceAlert>& alerts) {
    const auto& limits = config_.thresholds;
    const auto& process = sample.process;

    if (process.threadCount) {
        const double limit = limits.maxThreads;
        if (auto change = threads_.update(*process.threadCount, limit, limit * 0.1)) {
            std::ostringstream text;
            if (*change) {
                text << "Resources: the client runs " << *process.threadCount << " threads, over "
                     << limits.maxThreads << " - something is starting threads it does not end, "
                     << "and starting one more fails once their stacks no longer fit; " << line;
            } else {
                text << "Resources: the client is back to " << *process.threadCount
                     << " threads, under " << limits.maxThreads;
            }
            alerts.push_back({ResourceAlert::Kind::Threads, *change, text.str()});
        }
    }

    if (process.systemCommitUsedBytes && process.systemCommitLimitBytes &&
        *process.systemCommitLimitBytes > limits.minSystemCommitFreeBytes) {
        const double used = static_cast<double>(*process.systemCommitUsedBytes);
        const double limit =
            static_cast<double>(*process.systemCommitLimitBytes - limits.minSystemCommitFreeBytes);
        if (auto change = systemCommit_.update(used, limit, 512.0 * kMiB)) {
            const uint64_t free = *process.systemCommitLimitBytes -
                std::min(*process.systemCommitUsedBytes, *process.systemCommitLimitBytes);
            std::ostringstream text;
            text << "Resources: the system has " << formatBytes(free) << " of commit left ("
                 << formatBytes(*process.systemCommitUsedBytes) << " of "
                 << formatBytes(*process.systemCommitLimitBytes) << " used)";
            if (*change) {
                text << ", under " << formatBytes(limits.minSystemCommitFreeBytes)
                     << " - an allocation fails when it runs out and the client aborts with "
                        "bad_alloc; " << line;
            } else {
                text << ", back over " << formatBytes(limits.minSystemCommitFreeBytes);
            }
            alerts.push_back({ResourceAlert::Kind::SystemCommit, *change, text.str()});
        }
    }

    if (process.privateBytes && process.totalPhysicalBytes) {
        const double limit =
            static_cast<double>(*process.totalPhysicalBytes) * limits.maxPrivateShareOfPhysical;
        if (auto change = privateCommit_.update(static_cast<double>(*process.privateBytes),
                                                limit, limit * 0.1)) {
            std::ostringstream text;
            text << "Resources: the client has " << formatBytes(*process.privateBytes)
                 << " committed, " << (*change ? "over " : "back under ")
                 << static_cast<int>(limits.maxPrivateShareOfPhysical * 100.0) << "% of the "
                 << formatBytes(*process.totalPhysicalBytes) << " of physical memory";
            if (*change) text << "; " << line;
            alerts.push_back({ResourceAlert::Kind::PrivateCommit, *change, text.str()});
        }
    }
}

// ---- ResourceTelemetry ------------------------------------------------------

ResourceTelemetry::ResourceTelemetry(Config config) : config_(config) {
    series_.reserve(static_cast<size_t>(ResourceSeries::Count_));
    for (size_t i = 0; i < static_cast<size_t>(ResourceSeries::Count_); ++i) {
        series_.emplace_back(config_.historySamples);
    }
}

void ResourceTelemetry::noteFrame(float seconds) {
    sampleFrames_.seconds.push_back(seconds);
    logFrames_.seconds.push_back(seconds);
}

ResourceTelemetry::Recorded ResourceTelemetry::record(ResourceSample sample) {
    Recorded out;
    sample.frames = sampleFrames_.summary();
    sampleFrames_.seconds.clear();

    const auto push = [this](ResourceSeries which, float value) {
        series_[static_cast<size_t>(which)].push(value);
    };
    const auto& process = sample.process;
    push(ResourceSeries::FrameRate, sample.frames.frames ? sample.frames.averageFps : kNotMeasured);
    push(ResourceSeries::LatencyMs, orNotMeasured(sample.latencyMs));
    push(ResourceSeries::WorkingSetMB, orNotMeasured(process.workingSetBytes, kMiB));
    push(ResourceSeries::PrivateMB, orNotMeasured(process.privateBytes, kMiB));
    push(ResourceSeries::SystemCommitUsedMB, orNotMeasured(process.systemCommitUsedBytes, kMiB));
    push(ResourceSeries::Threads, orNotMeasured(process.threadCount));

    // The line the alerts carry is of the frames since the last line too:
    // that is the stretch the warning arrives at the end of.
    const std::string line = describe(sample, logFrames_.summary());
    checkThresholds(sample, line, out.alerts);

    if (!lastLogAt_) {
        lastLogAt_ = sample.timeSeconds;
    } else if (sample.timeSeconds - *lastLogAt_ >= config_.logIntervalSeconds) {
        out.logLine = line;
        logFrames_.seconds.clear();
        lastLogAt_ = sample.timeSeconds;
    }
    latest_ = std::move(sample);
    return out;
}

const ResourceSample* ResourceTelemetry::latest() const {
    return latest_ ? &*latest_ : nullptr;
}

const SampleRing& ResourceTelemetry::series(ResourceSeries which) const {
    return series_[static_cast<size_t>(which)];
}

std::string ResourceTelemetry::describe(const ResourceSample& sample, const FrameSummary& frames) {
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(1);
    line << "resources: ";
    if (frames.frames > 0) {
        line << frames.averageFps << " fps avg, " << frames.minimumFps << " min, p99 "
             << frames.p99FrameMs << " ms";
    } else {
        line << "no frames";
    }
    line << " | latency ";
    if (sample.latencyMs) line << *sample.latencyMs << " ms"; else line << "n/a";

    const auto& process = sample.process;
    line << " | working set " << bytesOrNone(process.workingSetBytes) << ", private "
         << bytesOrNone(process.privateBytes);
    line << " | system commit " << bytesOrNone(process.systemCommitUsedBytes) << " of "
         << bytesOrNone(process.systemCommitLimitBytes) << ", physical "
         << bytesOrNone(process.availablePhysicalBytes) << " free of "
         << bytesOrNone(process.totalPhysicalBytes);
    line << " | threads ";
    if (process.threadCount) line << *process.threadCount; else line << "n/a";

    for (const auto& cache : sample.caches) {
        line << " | " << cache.name << " " << formatBytes(cache.stats.bytes);
        if (cache.stats.budgetBytes > 0) line << " of " << formatBytes(cache.stats.budgetBytes);
        line << " (" << cache.stats.entries << ")";
    }
    for (const auto& count : sample.counts) {
        line << " | " << count.name << " " << count.value;
    }
    return line.str();
}

} // namespace wowee::core
