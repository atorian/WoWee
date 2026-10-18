#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace wowee {
namespace rendering {

/// The wind's sea as a whole, kept on the CPU beside the GPU's spectrum
/// (OceanWaves): how steep and how high it stands now, and the clock its
/// waves run on.
///
/// The GPU follows the wind wave vector by wave vector - each grows toward
/// what the wind asks of it, or dies away, over its own periods
/// (ocean_spectrum.glsli's oceanRelaxedVariance). How fast does not depend
/// on which way a wave runs, only on how long it is, so the sea's totals
/// follow the same way ring by ring of wavelength: kRings of them, each
/// holding the slope its waves carry. That is what says how far the water
/// may ride toward its crests without folding (a sea left by a gale is
/// steeper than the breeze after it asks for), and how high its crests
/// stand for the light through them.
class OceanSea {
public:
    static constexpr int kRings = 64;

    /// Move the sea on by `deltaSeconds` under a wind of `speedYardsPerSecond`
    /// (gust in) blowing toward `windDirection`. The first advance is the sea
    /// as that wind already has it.
    void advance(double deltaSeconds, float speedYardsPerSecond, glm::vec2 windDirection);

    /// The wind as last advanced: its speed, its way (unit), and what the
    /// spectrum is scaled by in it (oceanSlopeScale).
    [[nodiscard]] float speed() const { return speed_; }
    [[nodiscard]] glm::vec2 toward() const { return toward_; }
    [[nodiscard]] float slopeScale() const { return slopeScale_; }

    /// The sea as it stands: its mean square slope (both ways together),
    /// its height's variance in square yards, and how far its water rides
    /// toward the crests (oceanRide).
    [[nodiscard]] float meanSquareSlope() const;
    [[nodiscard]] float heightVariance() const;
    [[nodiscard]] float ride() const;

    /// The waves' clock: seconds, going round every kOceanRepeatSeconds,
    /// after which every wave is where it started.
    [[nodiscard]] float time() const { return static_cast<float>(time_); }

    /// How much of cascade `cascade`'s sea water `depth` yards deep and
    /// `fetch` yards downwind of the shore keeps, 0 to 1: of its height
    /// (`kind` kKeptHeight) or of its slope (kKeptSlope). Each of its waves
    /// keeps the share of its energy the TMA spectrum leaves it at that
    /// depth (oceanDepthFactor) and the fetch lets the wind raise
    /// (oceanFetchFactor), weighted by how much of the cascade's height or
    /// slope that wave carries now - a cascade's long waves lie down before
    /// its short ones, near the shore and in a pond. kOceanOpenFetch is the
    /// open sea.
    static constexpr int kKeptHeight = 0;
    static constexpr int kKeptSlope = 1;
    [[nodiscard]] float kept(int kind, int cascade, float depth, float fetch) const;
    /// The same at the table's depths (oceanShoalingDepth) and fetches
    /// (oceanFetchDistance), packed as the water's shaders read it: entry
    /// ((kind * cascades + cascade) * fetch rows + row) * depth steps + step,
    /// four to a vec4. Worked out when first asked for after an advance, so
    /// a frame that draws no water does not pay for it.
    static constexpr int kKeptEntries = 2 * 3 * 17 * 16;
    [[nodiscard]] const std::array<glm::vec4, kKeptEntries / 4>& keptTable() const;

private:
    void tabulateKept() const;

    std::array<float, kRings> slopeSq_{};
    mutable std::array<glm::vec4, kKeptEntries / 4> kept_{};
    mutable bool keptCurrent_ = false;
    double time_ = 0.0;
    float speed_ = 0.0f;
    glm::vec2 toward_{1.0f, 0.0f};
    float slopeScale_ = 0.0f;
    bool started_ = false;
};

} // namespace rendering
} // namespace wowee
