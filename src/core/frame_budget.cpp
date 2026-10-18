#include "core/frame_budget.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace wowee::core {

namespace {

/// The value at nearest rank `fraction` of an ascending run.
float rankOf(const std::vector<float>& ascending, double fraction) {
    const size_t n = ascending.size();
    const auto rank = static_cast<size_t>(std::ceil(fraction * static_cast<double>(n)));
    return ascending[std::min(n - 1, std::max<size_t>(rank, 1) - 1)];
}

std::string ms(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

std::string_view lastSegment(std::string_view name) {
    const size_t slash = name.rfind('/');
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

/// The stage `name` is part of, or empty for a stage of the frame itself.
std::string_view parentOf(std::string_view name) {
    const size_t slash = name.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : name.substr(0, slash);
}

} // namespace

TimeDistribution distributionOf(std::vector<float>& samplesMs) {
    TimeDistribution out;
    if (samplesMs.empty()) return out;
    std::sort(samplesMs.begin(), samplesMs.end());
    out.count = static_cast<uint32_t>(samplesMs.size());
    for (const float s : samplesMs) out.totalMs += s;
    out.meanMs = static_cast<float>(out.totalMs / out.count);
    out.p50Ms = rankOf(samplesMs, 0.50);
    out.p95Ms = rankOf(samplesMs, 0.95);
    out.p99Ms = rankOf(samplesMs, 0.99);
    out.maxMs = samplesMs.back();
    return out;
}

void FrameBudget::noteStage(std::string_view name, float milliseconds) {
    auto it = stages_.find(name);
    if (it == stages_.end()) it = stages_.emplace(std::string(name), Series{}).first;
    it->second.samplesMs.push_back(milliseconds);
    it->second.totalMs += milliseconds;
}

void FrameBudget::noteGpuFrame(const std::vector<std::pair<const char*, double>>& passes) {
    if (passes.empty()) return;
    double total = 0.0;
    for (const auto& [label, passMs] : passes) {
        auto it = std::find_if(gpuPasses_.begin(), gpuPasses_.end(),
                               [&](const auto& pass) { return pass.first == label; });
        if (it == gpuPasses_.end()) {
            gpuPasses_.emplace_back(label, Series{});
            it = gpuPasses_.end() - 1;
        }
        it->second.samplesMs.push_back(static_cast<float>(passMs));
        it->second.totalMs += passMs;
        total += passMs;
    }
    gpuFrameMs_.push_back(static_cast<float>(total));
    ++gpuFrames_;
}

void FrameBudget::frameFinished(float frameMilliseconds) {
    frameMs_.push_back(frameMilliseconds);
}

TimeDistribution FrameBudget::frameTimes() const {
    std::vector<float> samples = frameMs_;
    return distributionOf(samples);
}

uint32_t FrameBudget::framesOver(float limitMs) const {
    return static_cast<uint32_t>(
        std::count_if(frameMs_.begin(), frameMs_.end(), [&](float f) { return f > limitMs; }));
}

void FrameBudget::stageLines(const std::string& parent, int depth, double windowMs,
                             std::vector<std::string>& out) const {
    // Costliest first, each followed by what it is made of.
    std::vector<std::pair<double, const std::string*>> byCost;
    for (const auto& [name, series] : stages_) {
        const std::string_view up = parentOf(name);
        // A part whose whole was never timed stands on its own.
        const bool topLevel = up.empty() || stages_.find(up) == stages_.end();
        if (depth == 0 ? topLevel : up == parent) byCost.emplace_back(series.totalMs, &name);
    }
    std::sort(byCost.begin(), byCost.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    const uint32_t frameCount = std::max<uint32_t>(frames(), 1);
    for (const auto& [total, name] : byCost) {
        std::vector<float> samples = stages_.at(*name).samplesMs;
        const TimeDistribution d = distributionOf(samples);
        std::ostringstream line;
        line << std::string(2 + 2 * static_cast<size_t>(depth), ' ')
             << (depth == 0 ? std::string_view(*name) : lastSegment(*name)) << ": "
             << ms(total / frameCount) << "ms/frame avg, p95 " << ms(d.p95Ms) << ", worst "
             << ms(d.maxMs) << "ms, "
             << static_cast<int>(windowMs > 0.0 ? 100.0 * total / windowMs : 0.0) << "% of the window";
        out.push_back(line.str());
        stageLines(*name, depth + 1, windowMs, out);
    }
}

std::vector<std::string> FrameBudget::report(std::string_view title, double windowMs) const {
    std::vector<std::string> out;
    if (frameMs_.empty()) return out;

    const TimeDistribution frame = frameTimes();
    std::ostringstream head;
    head << title << ": " << frame.count << " frames, "
         << ms(windowMs > 0.0 ? frame.count * 1000.0 / windowMs : 0.0) << " fps; frame p50 "
         << ms(frame.p50Ms) << " / p95 " << ms(frame.p95Ms) << " / p99 " << ms(frame.p99Ms)
         << " / max " << ms(frame.maxMs) << "ms (avg " << ms(frame.meanMs) << ")";
    for (const float limit : kHitchLimitsMs) {
        char text[16];
        std::snprintf(text, sizeof(text), "%.1f", limit);
        head << ", over " << text << "ms: " << framesOver(limit);
    }
    out.push_back(head.str());

    stageLines({}, 0, windowMs, out);

    if (gpuFrames_ > 0) {
        std::vector<float> totals = gpuFrameMs_;
        const TimeDistribution gpu = distributionOf(totals);
        std::ostringstream gpuHead;
        gpuHead << "  GPU, " << gpuFrames_ << " frames read back: " << ms(gpu.meanMs)
                << "ms/frame avg, p95 " << ms(gpu.p95Ms) << ", worst " << ms(gpu.maxMs)
                << "ms across " << gpuPasses_.size() << " passes";
        out.push_back(gpuHead.str());
        for (const auto& [label, series] : gpuPasses_) {
            std::vector<float> samples = series.samplesMs;
            const TimeDistribution d = distributionOf(samples);
            std::ostringstream line;
            line << "    " << label << ": " << ms(series.totalMs / gpuFrames_) << "ms avg, p95 "
                 << ms(d.p95Ms) << ", worst " << ms(d.maxMs) << "ms";
            out.push_back(line.str());
        }
    }
    return out;
}

void FrameBudget::reset() {
    frameMs_.clear();
    stages_.clear();
    gpuPasses_.clear();
    gpuFrameMs_.clear();
    gpuFrames_ = 0;
}

} // namespace wowee::core
