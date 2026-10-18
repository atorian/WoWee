#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

/// What weather does to the camera's lens this frame.
struct LensInputs {
    float rain = 0.0f;  ///< WeatherMix::rain
    float snow = 0.0f;  ///< WeatherMix::snow
    float sand = 0.0f;  ///< WeatherMix::sand
    /// How open to the sky the camera is, 0 under a roof to 1 in the open
    /// (ShelterMap::exposure).
    float exposure = 1.0f;
    /// How squarely the lens faces into what falls, 0-1: looking up into
    /// rain, or into the wind in a sandstorm.
    float facingFall = 0.0f;
    float facingWind = 0.0f;
    /// Where the wind carries grit across the screen, in screen heights a
    /// second (+x right, +y down).
    glm::vec2 windOnScreen{0.0f};
    /// The camera is under water. Coming up out of it, water runs down the
    /// lens.
    bool underwater = false;
};

/// One thing on the lens. Positions are in screen uv (0-1, +y down); sizes in
/// screen heights.
struct LensDrop {
    enum class Kind : uint8_t { Water, Flake, Grit };
    Kind kind = Kind::Water;
    glm::vec2 pos{0.0f};
    float radius = 0.0f;
    glm::vec2 velocity{0.0f};  ///< screen heights a second
    float age = 0.0f;
    float life = 1.0f;
    float seed = 0.0f;         ///< 0-1, for the shaders' variation
    /// Seconds until a running drop leaves the next bead of water behind.
    float trailTimer = 0.0f;

    /// 0 when it is gone, 1 while it is whole.
    [[nodiscard]] float opacity() const;
    [[nodiscard]] bool running() const { return kind == Kind::Water && velocity.y > 0.0f; }
};

/// The rain, snow and sand on the camera's lens, as The Witcher 3 shows them:
/// drops gather while the camera stands in the rain looking into it, the big
/// ones run down and leave beads behind, they dry off under a roof; snow
/// sticks and melts to water and frosts the edges; sand drives across in the
/// wind and leaves a dusty film. Coming up out of water, it runs off.
///
/// Its own random sequence, so a run is the same each time for the same
/// inputs.
class LensDrops {
public:
    static constexpr size_t kMaxDrops = 192;
    /// A water drop at least this large runs down the lens.
    static constexpr float kRunRadius = 0.016f;

    explicit LensDrops(uint32_t seed = 0x9e3779b9u) : rng_(seed ? seed : 1u) { drops_.reserve(kMaxDrops); }

    /// `aspect` is the screen's width over its height.
    void update(float dt, const LensInputs& in, float aspect);
    void clear();

    [[nodiscard]] const std::vector<LensDrop>& drops() const { return drops_; }
    /// Frost round the edges, 0-1.
    [[nodiscard]] float frost() const { return frost_; }
    /// The dust film sand leaves, 0-1.
    [[nodiscard]] float dust() const { return dust_; }
    [[nodiscard]] bool empty() const { return drops_.empty() && frost_ <= 0.0f && dust_ <= 0.0f; }

private:
    float random();
    float random(float lo, float hi) { return lo + (hi - lo) * random(); }
    /// How many to start this frame at `perSecond`, carrying the fraction.
    int spawnCount(float perSecond, float dt, float& carry);
    void spawnWater(glm::vec2 pos, float radius, float life);
    void spawnFlake();
    void spawnGrit(const LensInputs& in);
    void mergeWater(float aspect);

    std::vector<LensDrop> drops_;
    uint32_t rng_;
    float rainCarry_ = 0.0f;
    float snowCarry_ = 0.0f;
    float gritCarry_ = 0.0f;
    float frost_ = 0.0f;
    float dust_ = 0.0f;
    bool wasUnderwater_ = false;
};

}  // namespace rendering
}  // namespace wowee
