#include "rendering/lightning_storm.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::rendering {
namespace {
constexpr float kLeaderSeconds = 0.09f;
constexpr float kTrailSeconds = 1.4f;
constexpr float kSoundYardsPerSecond = 375.0f;
}

LightningStorm::LightningStorm(uint32_t seed) : random_(seed) {}
float LightningStorm::random(float low, float high) {
    return std::uniform_real_distribution<float>(low, high)(random_);
}
float LightningStorm::interval() {
    return random(glm::mix(40.0f, 18.0f, intensity_), glm::mix(65.0f, 32.0f, intensity_));
}
void LightningStorm::setWeather(bool enabled, float intensity) {
    intensity_ = std::clamp(intensity, 0.0f, 1.0f);
    if (enabled && !enabled_) untilStrike_ = interval();
    enabled_ = enabled;
}
void LightningStorm::reset() {
    age_ = 100.0f;
    segments_.clear();
    sounds_.clear();
    arrivals_.clear();
    lastListener_.reset();
    untilStrike_ = interval();
}
void LightningStorm::update(float dt, const glm::vec3& listener, const Ground& ground) {
    dt = std::max(dt, 0.0f);
    // A teleport must not bring the previous location's storm or sound with it.
    if (lastListener_ && glm::distance(*lastListener_, listener) > 600.0f) reset();
    lastListener_ = listener;
    age_ += dt;
    for (auto it = sounds_.begin(); it != sounds_.end();) {
        it->age += dt;
        if (it->age >= kLeaderSeconds + glm::distance(listener, it->position) / kSoundYardsPerSecond) {
            arrivals_.push_back({it->position, it->variant});
            it = sounds_.erase(it);
        } else ++it;
    }
    if (!enabled_) return;
    untilStrike_ -= dt;
    if (untilStrike_ > 0.0f) return;
    untilStrike_ = interval();
    const float angle = random(0.0f, 6.283185307f);
    const float distance = random(90.0f, 260.0f);
    glm::vec3 hit = listener + glm::vec3(std::cos(angle) * distance, std::sin(angle) * distance, 0.0f);
    const auto height = ground ? ground(hit) : std::nullopt;
    if (!height) return; // the caller reports unavailable world geometry
    hit.z = *height;
    strike(hit + glm::vec3(random(-25.0f, 25.0f), random(-25.0f, 25.0f), random(120.0f, 200.0f)), hit);
}
void LightningStorm::strike(const glm::vec3& cloud, const glm::vec3& ground) {
    if (glm::distance(cloud, ground) < 1.0f) return;
    cloud_ = cloud;
    ground_ = ground;
    age_ = 0.0f;
    segments_.clear();
    sounds_.push_back({ground, 0.0f, static_cast<uint32_t>(random_( ) % 4)});
    const glm::vec3 direction = glm::normalize(ground - cloud);
    const glm::vec3 axis = std::abs(direction.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    const glm::vec3 across = glm::normalize(glm::cross(direction, axis));
    const glm::vec3 other = glm::cross(direction, across);
    glm::vec3 previous = cloud;
    constexpr int steps = 28;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        const float spread = std::sin(t * 3.14159265f) * glm::distance(cloud, ground) * 0.045f;
        const glm::vec3 point = i == steps ? ground : glm::mix(cloud, ground, t) +
            spread * (across * random(-1, 1) + other * random(-1, 1));
        const float begin = static_cast<float>(i - 1) / steps;
        segments_.push_back({previous, point, begin, t, 0.22f});
        if (i > 3 && i < 22 && random(0, 1) < 0.18f) {
            const glm::vec3 end = point + direction * random(10, 27) + across * random(-22, 22) + other * random(-18, 18);
            glm::vec3 branch = point;
            for (int j = 1; j <= 5; ++j) {
                const float b = static_cast<float>(j) / 5;
                const glm::vec3 next = glm::mix(point, end, b) + across * (std::sin(b * 3.14159265f) * random(-3, 3));
                segments_.push_back({branch, next, t, std::min(1.0f, t + 0.12f * b), 0.09f});
                branch = next;
            }
        }
        previous = point;
    }
}
bool LightningStorm::visible() const { return !segments_.empty() && age_ < kLeaderSeconds + kTrailSeconds; }
float LightningStorm::progress() const { return std::clamp(age_ / kLeaderSeconds, 0.0f, 1.0f); }
float LightningStorm::brightness() const {
    if (!visible()) return 0.0f;
    if (age_ < kLeaderSeconds) return 0.3f;
    const float t = (age_ - kLeaderSeconds) / kTrailSeconds;
    return std::exp(-3.0f * t) * (1.0f - t) * (1.0f - t);
}
float LightningStorm::illumination() const {
    if (!visible() || age_ < kLeaderSeconds) return 0.0f;
    const float t = age_ - kLeaderSeconds;
    return std::exp(-t / 0.16f) * (1.0f - t / kTrailSeconds);
}
std::vector<ThunderArrival> LightningStorm::takeThunder() {
    std::vector<ThunderArrival> result;
    result.swap(arrivals_);
    return result;
}
} // namespace wowee::rendering
