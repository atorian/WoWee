#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <vector>

namespace wowee::rendering {

struct LightningSegment {
    glm::vec3 start, end;
    float begin, finish; // fraction of the downward leader's journey
    float width;
};

struct ThunderArrival {
    glm::vec3 position;
    uint32_t variant;
};

/// The lifetime of a world-space strike, shared by its drawing, light and sound.
/// Independent of Vulkan and audio; the client consumes arrivals exactly once.
class LightningStorm {
public:
    using Ground = std::function<std::optional<float>(const glm::vec3&)>;
    explicit LightningStorm(uint32_t seed);
    void setWeather(bool enabled, float intensity);
    void update(float dt, const glm::vec3& listener, const Ground& ground);
    void strike(const glm::vec3& cloud, const glm::vec3& ground);
    void reset();
    void reseed(uint32_t seed) { random_.seed(seed); reset(); }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] float intensity() const { return intensity_; }
    [[nodiscard]] bool visible() const;
    [[nodiscard]] float progress() const;
    [[nodiscard]] float brightness() const;
    [[nodiscard]] float illumination() const;
    [[nodiscard]] const std::vector<LightningSegment>& segments() const { return segments_; }
    [[nodiscard]] glm::vec3 lightPosition() const { return glm::mix(ground_, cloud_, 0.65f); }
    [[nodiscard]] glm::vec3 groundPosition() const { return ground_; }
    [[nodiscard]] std::vector<ThunderArrival> takeThunder();

private:
    float random(float low, float high);
    float interval();
    struct Sound { glm::vec3 position; float age; uint32_t variant; };
    std::mt19937 random_;
    bool enabled_ = false;
    float intensity_ = 0.0f;
    float untilStrike_ = 0.0f;
    float age_ = 100.0f;
    glm::vec3 cloud_{0.0f}, ground_{0.0f};
    std::optional<glm::vec3> lastListener_;
    std::vector<LightningSegment> segments_;
    std::vector<Sound> sounds_;
    std::vector<ThunderArrival> arrivals_;
};

} // namespace wowee::rendering
