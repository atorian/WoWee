#include "rendering/liquid_ripple_frames.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

namespace {

// More frames than any liquid has, so a pattern whose every number resolves
// - a stand-in file served for any path - cannot load forever.
constexpr uint32_t kMaxFrames = 128;

/// How much a channel varies over a frame: a channel that holds the ripple
/// varies, one that does not is flat.
double channelSpread(const pipeline::BLPImage& image, int channel) {
    const size_t texels = static_cast<size_t>(image.width) * image.height;
    if (texels == 0 || image.data.size() < texels * 4) return 0.0;
    double sum = 0.0, sumSq = 0.0;
    for (size_t i = 0; i < texels; ++i) {
        const double v = image.data[i * 4 + channel];
        sum += v;
        sumSq += v * v;
    }
    const double mean = sum / static_cast<double>(texels);
    return std::sqrt(std::max(0.0, sumSq / static_cast<double>(texels) - mean * mean));
}

void appendRipple(const pipeline::BLPImage& image, bool fromAlpha, std::vector<uint8_t>& out) {
    const size_t texels = static_cast<size_t>(image.width) * image.height;
    out.reserve(out.size() + texels);
    for (size_t i = 0; i < texels; ++i) {
        const uint8_t* p = &image.data[i * 4];
        if (fromAlpha) {
            out.push_back(p[3]);
        } else {
            out.push_back(static_cast<uint8_t>((p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8));
        }
    }
}

/// Every frame at half the size, averaging each 2x2 block - layer by layer,
/// since the frames lie one after another.
std::vector<uint8_t> halve(const std::vector<uint8_t>& level, uint32_t w, uint32_t h,
                           uint32_t frames) {
    const uint32_t nw = std::max(1u, w / 2);
    const uint32_t nh = std::max(1u, h / 2);
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * frames);
    for (uint32_t f = 0; f < frames; ++f) {
        const uint8_t* src = level.data() + static_cast<size_t>(f) * w * h;
        uint8_t* dst = out.data() + static_cast<size_t>(f) * nw * nh;
        for (uint32_t y = 0; y < nh; ++y) {
            for (uint32_t x = 0; x < nw; ++x) {
                const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                const uint32_t s = src[y0 * w + x0] + src[y0 * w + x1] + src[y1 * w + x0] +
                                   src[y1 * w + x1];
                dst[y * nw + x] = static_cast<uint8_t>((s + 2) / 4);
            }
        }
    }
    return out;
}

} // namespace

float rippleShareForWind(float speedYardsPerSecond) {
    constexpr float kYardsPerMetre = 1.0936133f;
    constexpr float kRipplesStart = 0.5f * kYardsPerMetre;  // Beaufort 1, light air
    constexpr float kRipplesFull = 3.4f * kYardsPerMetre;   // Beaufort 3, gentle breeze
    const float t = std::clamp((speedYardsPerSecond - kRipplesStart) / (kRipplesFull - kRipplesStart),
                               0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::string liquidFramePath(const std::string& pattern, uint32_t frame) {
    const size_t at = pattern.find("%d");
    if (at == std::string::npos) return pattern;
    return pattern.substr(0, at) + std::to_string(frame) + pattern.substr(at + 2);
}

std::optional<LiquidRippleFrames> loadLiquidRippleFrames(
        const std::string& pattern,
        const std::function<pipeline::BLPImage(const std::string&)>& loadFrame,
        std::string& whyNot) {
    if (pattern.empty()) {
        whyNot = "the liquid names no texture";
        return std::nullopt;
    }
    const bool animated = pattern.find("%d") != std::string::npos;

    LiquidRippleFrames set;
    std::vector<uint8_t> base;
    for (uint32_t frame = 1; frame <= kMaxFrames; ++frame) {
        const std::string path = liquidFramePath(pattern, frame);
        const pipeline::BLPImage image = loadFrame(path);
        if (!image.isValid() || image.data.empty()) {
            if (frame == 1) {
                whyNot = "its first frame, " + path + ", did not load";
                return std::nullopt;
            }
            break;
        }
        if (frame == 1) {
            set.width = static_cast<uint32_t>(image.width);
            set.height = static_cast<uint32_t>(image.height);
            set.fromAlpha = channelSpread(image, 3) > channelSpread(image, 0);
        } else if (static_cast<uint32_t>(image.width) != set.width ||
                   static_cast<uint32_t>(image.height) != set.height) {
            LOG_WARNING("Water: ", path, " is ", image.width, "x", image.height, ", not ",
                        set.width, "x", set.height, " like the frames before it - the ripple "
                        "of ", pattern, " loops over the first ", set.frames, " frames");
            break;
        }
        appendRipple(image, set.fromAlpha, base);
        ++set.frames;
        if (!animated) break;
    }

    uint32_t w = set.width, h = set.height;
    set.levels.push_back(std::move(base));
    while (w > 1 || h > 1) {
        set.levels.push_back(halve(set.levels.back(), w, h, set.frames));
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return set;
}

} // namespace rendering
} // namespace wowee
