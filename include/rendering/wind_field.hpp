#pragma once

#include "pipeline/weather_kind.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace wowee {
namespace pipeline {
struct WindZoneRow;
class WindZoneTable;
}  // namespace pipeline

namespace rendering {

/// How strong the realm's weather must be before it changes the wind - the
/// strength the game itself counts as raining.
inline constexpr float kWindWeatherThreshold = 0.05f;

/// The wind one place has at one moment, before anything smooths it: what
/// the zone's row and the weather say, as a pure function of the clock.
struct BaseWind {
    float directionDegrees = 0.0f;  ///< toward, render ground plane (wind_zones.hpp)
    float speed = 0.0f;             ///< yards per second
    float gust = 1.0f;              ///< what a gust multiplies the speed by; 1 is none
    size_t regime = 0;              ///< index into the table's regimes
};

/// The wind the table says blows over `row` at `clockSeconds` in this
/// weather. Every client of a realm passes the same realm second and the same
/// row and gets the same answer: that is the whole of the wind's agreement
/// between clients, and why nothing here may read a clock or a random source
/// of its own.
///
/// Time is cut into slots of the row's mean dwell, and each slot draws a
/// regime from the row's weights by a hash of (row, slot) - O(1) for any
/// moment, past or future. The weather's rule favours or forces a regime when
/// the realm's weather is at least kWindWeatherThreshold strong.
///
/// The speed is the middle of the regime's range and the direction the row's,
/// and there are no gusts: with only a default row, the wind is constant. The
/// slot and the hash are where the next step hangs jittered slot boundaries,
/// a speed drawn within the range, a veering direction and discrete gust
/// fronts - each a hash of the slot it falls in, so it stays O(1) and the same
/// on every client.
[[nodiscard]] BaseWind baseWindAt(double clockSeconds, const pipeline::WindZoneRow& row,
                                  const pipeline::WindZoneTable& table, pipeline::WeatherKind weatherKind,
                                  float weatherIntensity);

/// A fixed wind, for a scene that wants to draw a particular one.
struct WindOverride {
    float directionDegrees = 0.0f;
    float speed = 0.0f;  ///< yards per second
};

/// What the wind is made from this frame.
struct WindInputs {
    /// The realm's clock in seconds since 1970, or the local clock when there
    /// is no realm. Only differences and slots of it are used, so a local
    /// clock gives a wind that is right but not shared.
    double clockSeconds = 0.0;
    /// The weather being drawn.
    pipeline::WeatherKind weatherKind = pipeline::WeatherKind::Clear;
    float weatherIntensity = 0.0f;
    /// Where the player stands, render xy. The grass's travelling fronts turn
    /// about it when the wind turns: a front measured from the world's origin
    /// would sweep across a field thousands of yards out at the rate the wind
    /// turns times that distance.
    glm::dvec2 pivot{0.0};
    /// Replaces what the table says, for an offline scene.
    std::optional<WindOverride> fixedWind;
};

/// The wind as everything that moves in it reads it.
struct WindState {
    static constexpr float maxLean = 0.3f;
    static constexpr float maxSwing = 2.5f;
    glm::vec2 direction{1.0f, 0.0f};  ///< toward, render xy, unit
    float speed = 0.0f;               ///< yards per second, smoothed
    float gust = 1.0f;                ///< multiplies the speed while a gust passes

    // How things answer it, from the speed and the gust: 1 at kReferenceSpeed,
    // the speed the shader layers were tuned at.
    float lean = 0.0f;    ///< how far downwind foliage leans, in units of its amplitude
    float swing = 0.0f;   ///< trunk, cloth and grass amplitude scale
    float detail = 0.0f;  ///< branch and leaf amplitude scale

    // Accumulated, never computed as time times a rate: a change of speed
    // changes how fast these advance from where they are, so nothing jumps.
    // Phases are wrapped to [0, kPhaseWrap), distances to [0, kTravelWrap)
    // (see wind.glsli for why those two).
    double trunkPhase = 0.0;
    double branchPhase = 0.0;
    double leafPhase = 0.0;
    double clothPhase = 0.0;
    double rustlePhase = 0.0;
    double waveTravel = 0.0;  ///< yards the grass fronts have travelled downwind
    double gustTravel = 0.0;  ///< yards the grass swells have travelled
};

/// What the shaders are handed (GPUPerFrameData's wind, wind.glsli).
struct WindGpu {
    glm::vec4 dirLean{1.0f, 0.0f, 0.0f, 0.0f};  ///< xy toward, z lean, w swing
    glm::vec4 phases{0.0f};                     ///< trunk, branch, leaf, detail
    glm::vec4 travel{0.0f};                     ///< cloth phase, fronts, swells, rustle
};

/// The world direction `dirWorld` in the xy of a model drawn with `model`,
/// whose sway is applied before its model matrix: the inverse rotation is the
/// transpose, and the scale it keeps is normalised away. The shadow pass's
/// half of windDirectionInModel in wind.glsli, which the main pass runs on
/// the GPU; the two must give one answer for a tree and its shadow to move
/// together.
inline glm::vec2 windDirectionInModel(const glm::mat4& model, const glm::vec2& dirWorld) {
    const glm::vec3 d = glm::transpose(glm::mat3(model)) * glm::vec3(dirWorld, 0.0f);
    const float len = glm::length(glm::vec2(d));
    return len > 1e-6f ? glm::vec2(d) / len : glm::vec2(1.0f, 0.0f);
}

/// What the shadow passes carry of the wind for one caster, read back by
/// foliageWindFromShadowPush in wind.glsli: the frame's wind with its
/// direction turned into the caster's own space (M2ShadowInstance::wind), and
/// the phases every caster shares (M2ShadowPush::windPhases).
struct ShadowCasterWind {
    glm::vec4 wind{0.0f};    ///< xy toward in the model's xy, z lean, w swing
    glm::vec4 phases{0.0f};  ///< trunk, branch, leaf, detail
};

inline ShadowCasterWind shadowWindFor(const WindGpu& wind, const glm::mat4& model) {
    return {glm::vec4(windDirectionInModel(model, glm::vec2(wind.dirLean)), wind.dirLean.z,
                      wind.dirLean.w),
            wind.phases};
}

/// The wind over the place the player is: one field for the trees, their
/// shadows, the cloth and the grass.
///
/// Its target is baseWindAt for the realm's clock and the zone, or a scene's
/// override. Arriving at a new target - a zone crossed, the weather turning -
/// it eases over seconds: the speed settles within kSpeedSettleSeconds of any
/// change up to 14 yd/s, the direction within kDirectionSettleSeconds of any
/// turn, always the short way round, and no frame moves either by more than
/// (change / time constant + floor rate) * dt. The first update takes the
/// target at once, so a scene draws the wind it asked for from its first frame.
///
/// The phases start where rate * clock puts them and then accumulate. With a
/// steady wind that keeps two clients of a realm in step with each other; it
/// is also taken again whenever the clock jumps - the realm's time arriving
/// after the local clock stood in, or the server's Unix time replacing the
/// coarser login date - so the realm's time decides it, not when this client
/// happened to start.
class WindField {
public:
    /// The speed the shader layers were tuned at: the middle of the "light"
    /// regime. At this speed everything moves as it did before there was a
    /// wind field, along one direction now instead of none.
    static constexpr float kReferenceSpeed = 2.0f;
    static constexpr float kSpeedTimeConstant = 3.0f;
    static constexpr float kSpeedFloorRate = 0.1f;          ///< yd/s per second
    static constexpr float kSpeedSettleSeconds = 15.0f;
    static constexpr float kDirectionTimeConstant = 4.0f;
    static constexpr float kDirectionFloorRate = 0.02f;     ///< radians per second
    static constexpr float kDirectionSettleSeconds = 20.0f;
    /// A clock that moves by more than this beyond the frame's own time has
    /// jumped, and the phases are taken from it again.
    static constexpr double kClockJumpSeconds = 1.0;
    static constexpr double kPhaseWrap = 20.0 * 3.14159265358979323846;
    static constexpr double kTravelWrap = 200.0 * 3.14159265358979323846;

    /// Advance by `dt` seconds toward the wind `row` has (or the override).
    void update(float dt, const WindInputs& inputs, const pipeline::WindZoneRow& row,
                const pipeline::WindZoneTable& table);

    [[nodiscard]] const WindState& state() const { return state_; }
    [[nodiscard]] WindGpu gpu() const;

private:
    void anchorPhases(double clockSeconds);
    void respond();

    WindState state_;
    BaseWind target_;
    bool started_ = false;
    double lastClock_ = 0.0;
};

}  // namespace rendering
}  // namespace wowee
