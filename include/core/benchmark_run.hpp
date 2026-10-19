#pragma once

#include "core/frame_budget.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace core {

/// A fixed flight through a place, the same on every run, so two builds -
/// or a build before and after a change - are measured on the same frames.
struct BenchmarkRoute {
    std::string name;
    uint32_t mapId = 0;
    uint32_t zoneId = 0;
    /// Where the character stands, as .gps gives it: the world streams in
    /// around this point, and the light, shadows and fog centre on it.
    glm::vec3 spawn{0.0f};
    /// The realm clock the flight is lit at.
    float hour = 12.0f;
    /// The camera's loop, as .gps positions: where it is and what it looks at
    /// at each point. It flies from the first round to the first again.
    struct Waypoint {
        glm::vec3 eye{0.0f};
        glm::vec3 lookAt{0.0f};
    };
    std::vector<Waypoint> loop;
};

/// The routes WOWEE_BENCHMARK flies: laps of dalaran, stormwind and
/// orgrimmar, and bootybay, a crossing that streams terrain the whole way.
[[nodiscard]] const std::vector<BenchmarkRoute>& benchmarkRoutes();
[[nodiscard]] const BenchmarkRoute* findBenchmarkRoute(std::string_view name);

/// A camera in render coordinates, aimed as Camera::setRotation takes it.
struct BenchmarkCamera {
    glm::vec3 position{0.0f};
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
};

/// Where the camera is `fraction` of the way round the loop (0 and 1 are the
/// first waypoint): a smooth curve through every waypoint, looking at a
/// point that moves along a curve through theirs.
[[nodiscard]] BenchmarkCamera cameraAlong(const BenchmarkRoute& route, float fraction);

/// One run of WOWEE_BENCHMARK: waits for the world, lets it settle, flies the
/// loop once over a fixed time and keeps the frame budget of the flight.
///
/// The client tells it each frame how long the frame took and what the
/// world is doing; it says where the camera goes and when the flight is
/// over. Nothing is recorded before the flight, so loading, streaming in and
/// the first frames of shader compilation are not in the numbers.
class BenchmarkRun {
public:
    struct Timing {
        /// After the world is in view, at least this long before the flight,
        /// so streaming and the first pipelines are behind it...
        float settleSeconds = 10.0f;
        /// ...and the flight starts after this long whether or not terrain is
        /// still streaming in. The report says when it was.
        float maxSettleSeconds = 60.0f;
        /// The loop, once, in this long.
        float flySeconds = 60.0f;
    };
    enum class Phase { WaitingForWorld, Settling, Flying, Finished };

    BenchmarkRun(BenchmarkRoute route, Timing timing);

    /// The end of a frame `frameSeconds` long, with the world in view or
    /// not, and still streaming in or not.
    void frameEnded(float frameSeconds, bool worldInView, bool streaming);

    [[nodiscard]] Phase phase() const { return phase_; }
    /// Whether this frame belongs to the flight, and its timings to the report.
    [[nodiscard]] bool recording() const { return phase_ == Phase::Flying; }
    /// The camera for this frame; none outside the flight.
    [[nodiscard]] std::optional<BenchmarkCamera> camera() const;
    [[nodiscard]] FrameBudget& budget() { return budget_; }
    [[nodiscard]] const BenchmarkRoute& route() const { return route_; }
    [[nodiscard]] float flownSeconds() const { return flown_; }

    /// A count of what is drawn (M2 instances, terrain tiles...) during the
    /// flight; the report gives its average and its most.
    void noteSceneCount(std::string_view name, uint64_t value);

    /// The report: `setup` (device, resolution, settings), the frame budget
    /// of the flight and the scene it flew through.
    [[nodiscard]] std::vector<std::string> report(const std::vector<std::string>& setup) const;
    /// One line for a log or a table of runs.
    [[nodiscard]] std::string summary() const;

private:
    struct SceneCount {
        std::string name;
        double total = 0.0;
        uint64_t most = 0;
        uint32_t samples = 0;
    };

    BenchmarkRoute route_;
    Timing timing_;
    Phase phase_ = Phase::WaitingForWorld;
    float settled_ = 0.0f;
    float flown_ = 0.0f;
    bool streamingAtTakeoff_ = false;
    FrameBudget budget_;
    std::vector<SceneCount> scene_;
};

/// Write a report to `dir`/benchmark-<route>-<YYYYMMDD-HHMMSS>.txt. The path
/// written, or none when it could not be - for the caller to say.
std::optional<std::filesystem::path> writeBenchmarkReport(const std::filesystem::path& dir,
                                                         std::string_view route,
                                                         const std::vector<std::string>& lines);

} // namespace core
} // namespace wowee
