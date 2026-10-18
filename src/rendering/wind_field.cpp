#include "rendering/wind_field.hpp"

#include "core/logger.hpp"
#include "pipeline/wind_zones.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace wowee {
namespace rendering {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// FNV-1a: the row's name as a key, the same on every client and through
/// any reordering of the file.
uint64_t nameKey(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

/// splitmix64's finaliser: every bit of the input reaches every bit out.
uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// A uniform number in [0, 1) for this key and slot.
double unitHash(uint64_t key, int64_t slot) {
    const uint64_t h = mix(key ^ mix(static_cast<uint64_t>(slot)));
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

double wrap(double v, double period) {
    v = std::fmod(v, period);
    return v < 0.0 ? v + period : v;
}

/// One frame's step from `diff` toward zero: exponential with time constant
/// `tau`, but never slower than `floorRate`, so it arrives rather than
/// approaching for ever - and never past.
double easeStep(double diff, double dt, double tau, double floorRate) {
    double step = diff * (1.0 - std::exp(-dt / tau));
    const double floorStep = floorRate * dt;
    if (std::abs(step) < floorStep) step = std::copysign(std::min(std::abs(diff), floorStep), diff);
    return step;
}

/// How fast each layer advances at a wind of `s` reference speeds. At s = 1
/// these are exactly the rates the shaders ran on the frame clock before
/// there was a wind: 0.8, 1.7 and 4.5 radians a second for trunk, branch and
/// leaf, 1.1 for cloth, 1.9 for the grass rustle, and the grass fronts and
/// swells travelling at 2.1 / 0.48 and 0.7 / 0.11 yards a second - the rates
/// at their spatial frequencies in wind.glsli.
///
/// A trunk is a pendulum whose rate the wind barely moves; a leaf flutters as
/// fast as the air goes past it; the rest lie between.
struct Rates {
    double trunk, branch, leaf, cloth, rustle, fronts, swells;
};

Rates ratesFor(double s) {
    s = std::max(s, 0.0);
    const double s34 = std::pow(s, 0.75);
    return {0.8 * std::sqrt(s), 1.7 * s34, 4.5 * s, 1.1 * s34, 1.9 * s34,
            (2.1 / 0.48) * s34, (0.7 / 0.11) * s};
}

}  // namespace

BaseWind baseWindAt(double clockSeconds, const pipeline::WindZoneRow& row,
                    const pipeline::WindZoneTable& table, pipeline::WeatherKind weatherKind,
                    float weatherIntensity) {
    const auto& regimes = table.regimes();
    BaseWind out;
    out.directionDegrees = row.directionDegrees;

    const pipeline::WindWeatherRule rule = weatherIntensity >= kWindWeatherThreshold
                                               ? table.weatherRule(weatherKind)
                                               : pipeline::WindWeatherRule{};
    const size_t count = std::min(regimes.size(), row.weights.size());
    if (rule.force && *rule.force < regimes.size()) {
        out.regime = *rule.force;
    } else if (count > 0) {
        // Favoured: drawn at least as often as all the others together.
        auto weightOf = [&](size_t i) {
            const float w = row.weights[i];
            if (!rule.favour || *rule.favour != i) return w;
            float others = 0.0f;
            for (size_t j = 0; j < count; ++j) {
                if (j != i) others += row.weights[j];
            }
            return std::max(w, others);
        };
        float total = 0.0f;
        for (size_t i = 0; i < count; ++i) total += weightOf(i);

        // The slot this moment falls in, and the regime it draws.
        const double slotSeconds =
            std::max(0.5 * (row.dwellMinSeconds + row.dwellMaxSeconds), 1.0);
        const auto slot = static_cast<int64_t>(std::floor(clockSeconds / slotSeconds));
        const double pick = unitHash(nameKey(row.name), slot) * total;
        double below = 0.0;
        out.regime = count - 1;
        for (size_t i = 0; i < count; ++i) {
            below += weightOf(i);
            if (pick < below) {
                out.regime = i;
                break;
            }
        }
    }
    if (out.regime < regimes.size()) {
        const auto& regime = regimes[out.regime];
        out.speed = 0.5f * (regime.speedMin + regime.speedMax);
    }
    return out;
}

void WindField::update(float dt, const WindInputs& inputs, const pipeline::WindZoneRow& row,
                       const pipeline::WindZoneTable& table) {
    if (inputs.fixedWind) {
        target_ = BaseWind{};
        target_.directionDegrees = inputs.fixedWind->directionDegrees;
        target_.speed = std::max(inputs.fixedWind->speed, 0.0f);
    } else {
        target_ = baseWindAt(inputs.clockSeconds, row, table, inputs.weatherKind,
                             inputs.weatherIntensity);
    }
    const double targetAngle = glm::radians(static_cast<double>(target_.directionDegrees));

    if (!started_) {
        started_ = true;
        state_.direction = glm::vec2(std::cos(targetAngle), std::sin(targetAngle));
        state_.speed = target_.speed;
        state_.gust = target_.gust;
        respond();
        anchorPhases(inputs.clockSeconds);
        lastClock_ = inputs.clockSeconds;
        LOG_INFO("Wind: toward ", target_.directionDegrees, " degrees at ", target_.speed,
                 " yd/s", inputs.fixedWind ? " (fixed by the scene)" : "");
        return;
    }

    const double step = std::max(static_cast<double>(dt), 0.0);

    // Speed, and the gust riding on it.
    state_.speed += static_cast<float>(easeStep(target_.speed - state_.speed, step,
                                                kSpeedTimeConstant, kSpeedFloorRate));
    state_.gust += static_cast<float>(easeStep(target_.gust - state_.gust, step,
                                               kSpeedTimeConstant, kSpeedFloorRate));

    // Direction, the short way round.
    const glm::dvec2 oldDir(state_.direction);
    const double angle = std::atan2(oldDir.y, oldDir.x);
    const double turn = std::remainder(targetAngle - angle, 2.0 * kPi);
    const double newAngle = angle + easeStep(turn, step, kDirectionTimeConstant, kDirectionFloorRate);
    const glm::dvec2 newDir(std::cos(newAngle), std::sin(newAngle));
    state_.direction = glm::vec2(newDir);

    // The grass fronts are measured along the wind from the world's origin;
    // turning them about the player instead keeps the field near the player
    // where it was.
    const double pivotShift = glm::dot(inputs.pivot, newDir - oldDir);
    state_.waveTravel = wrap(state_.waveTravel + pivotShift, kTravelWrap);
    state_.gustTravel = wrap(state_.gustTravel + pivotShift, kTravelWrap);

    respond();

    // A clock that moved by more than the frame did has jumped: the realm's
    // time arrived, or a finer one replaced a coarser one. The phases are
    // taken from it again rather than run on from a start that no other
    // client shares.
    const double expected = lastClock_ + step;
    lastClock_ = inputs.clockSeconds;
    if (std::abs(inputs.clockSeconds - expected) > kClockJumpSeconds) {
        LOG_INFO("Wind: the clock jumped by ", inputs.clockSeconds - expected,
                 " s; the sway phases are taken from it again");
        anchorPhases(inputs.clockSeconds);
        return;
    }

    const Rates r = ratesFor(state_.speed * state_.gust / kReferenceSpeed);
    state_.trunkPhase = wrap(state_.trunkPhase + r.trunk * step, kPhaseWrap);
    state_.branchPhase = wrap(state_.branchPhase + r.branch * step, kPhaseWrap);
    state_.leafPhase = wrap(state_.leafPhase + r.leaf * step, kPhaseWrap);
    state_.clothPhase = wrap(state_.clothPhase + r.cloth * step, kPhaseWrap);
    state_.rustlePhase = wrap(state_.rustlePhase + r.rustle * step, kPhaseWrap);
    state_.waveTravel = wrap(state_.waveTravel + r.fronts * step, kTravelWrap);
    state_.gustTravel = wrap(state_.gustTravel + r.swells * step, kTravelWrap);
}

void WindField::anchorPhases(double clockSeconds) {
    // Where a steady wind of this speed would have each layer at this moment:
    // the same on every client reading the same clock.
    const Rates r = ratesFor(state_.speed * state_.gust / kReferenceSpeed);
    state_.trunkPhase = wrap(clockSeconds * r.trunk, kPhaseWrap);
    state_.branchPhase = wrap(clockSeconds * r.branch, kPhaseWrap);
    state_.leafPhase = wrap(clockSeconds * r.leaf, kPhaseWrap);
    state_.clothPhase = wrap(clockSeconds * r.cloth, kPhaseWrap);
    state_.rustlePhase = wrap(clockSeconds * r.rustle, kPhaseWrap);
    state_.waveTravel = wrap(clockSeconds * r.fronts, kTravelWrap);
    state_.gustTravel = wrap(clockSeconds * r.swells, kTravelWrap);
}

void WindField::respond() {
    // In reference speeds. The lean saturates - a tree bends further in a gale
    // than a breeze, but not without limit - and the swings grow more slowly
    // than the speed: amplitude is the square root of the push, near enough.
    const float s = std::max(state_.speed * state_.gust, 0.0f) / kReferenceSpeed;
    state_.lean = 0.3f * s / (1.0f + s);
    state_.swing = std::min(std::sqrt(s), 2.5f);
    state_.detail = std::min(std::pow(s, 0.75f), 3.5f);
}

WindGpu WindField::gpu() const {
    WindGpu g;
    g.dirLean = glm::vec4(state_.direction, state_.lean, state_.swing);
    g.phases = glm::vec4(static_cast<float>(state_.trunkPhase), static_cast<float>(state_.branchPhase),
                         static_cast<float>(state_.leafPhase), state_.detail);
    g.travel = glm::vec4(static_cast<float>(state_.clothPhase), static_cast<float>(state_.waveTravel),
                         static_cast<float>(state_.gustTravel), static_cast<float>(state_.rustlePhase));
    return g;
}

}  // namespace rendering
}  // namespace wowee
