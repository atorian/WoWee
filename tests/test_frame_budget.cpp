// What the frame budget says about a stretch of frames: how the frame time is
// spread and how many frames missed 120, 60 and 30 fps, each stage's average
// beside its tail, the parts of a stage under it, and the GPU's passes
// averaged over every frame read back rather than the last one.
#include <catch_amalgamated.hpp>

#include "core/frame_budget.hpp"

#include <string>
#include <vector>

using wowee::core::FrameBudget;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

/// The index of the first line containing `part`, or -1.
int lineWith(const std::vector<std::string>& lines, const std::string& part) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (contains(lines[i], part)) return static_cast<int>(i);
    }
    return -1;
}

/// The leading spaces of a line.
size_t indentOf(const std::string& line) { return line.find_first_not_of(' '); }

} // namespace

TEST_CASE("the frame time is reported as its spread and the frames that missed each rate",
          "[frame budget]") {
    FrameBudget budget;
    // Ninety smooth frames, nine at half the rate and one hitch.
    for (int i = 0; i < 90; ++i) budget.frameFinished(10.0f);
    for (int i = 0; i < 9; ++i) budget.frameFinished(20.0f);
    budget.frameFinished(50.0f);

    const auto lines = budget.report("frame budget over 1s", 1000.0);
    REQUIRE_FALSE(lines.empty());
    const std::string& head = lines.front();
    CHECK(contains(head, "frame budget over 1s: 100 frames, 100.00 fps"));
    CHECK(contains(head, "p50 10.00 / p95 20.00 / p99 20.00 / max 50.00ms"));
    CHECK(contains(head, "over 8.3ms: 100"));
    CHECK(contains(head, "over 16.7ms: 10"));
    CHECK(contains(head, "over 33.3ms: 1"));
}

TEST_CASE("a stage's tail is reported beside its average", "[frame budget]") {
    FrameBudget budget;
    for (int i = 0; i < 19; ++i) {
        budget.noteStage("beginFrame", 2.0f);
        budget.frameFinished(16.0f);
    }
    budget.noteStage("beginFrame", 40.0f);
    budget.frameFinished(50.0f);

    const auto lines = budget.report("window", 1000.0);
    const int at = lineWith(lines, "beginFrame:");
    REQUIRE(at >= 0);
    // (19 * 2 + 40) / 20 frames.
    CHECK(contains(lines[at], "3.90ms/frame avg, p95 2.00, worst 40.00ms"));
}

TEST_CASE("the parts of a stage are reported under it, costliest first", "[frame budget]") {
    FrameBudget budget;
    budget.noteStage("Render", 20.0f);
    budget.noteStage("beginFrame", 12.0f);
    budget.noteStage("beginFrame/acquire", 1.0f);
    budget.noteStage("beginFrame/wait for GPU", 9.0f);
    budget.noteStage("beginFrame/graph", 2.0f);
    budget.noteStage("beginFrame/graph/shadow_pass", 1.5f);
    budget.noteStage("endFrame/present", 3.0f);
    budget.frameFinished(25.0f);

    const auto lines = budget.report("window", 25.0);
    const int begin = lineWith(lines, "beginFrame:");
    const int wait = lineWith(lines, "wait for GPU:");
    const int graph = lineWith(lines, "graph:");
    const int shadow = lineWith(lines, "shadow_pass:");
    const int acquire = lineWith(lines, "acquire:");
    REQUIRE(begin >= 0);
    REQUIRE(wait >= 0);
    REQUIRE(graph >= 0);
    REQUIRE(shadow >= 0);
    REQUIRE(acquire >= 0);
    // Under beginFrame, by cost, each followed by its own parts.
    CHECK(begin < wait);
    CHECK(wait < graph);
    CHECK(graph < shadow);
    CHECK(shadow < acquire);
    CHECK(indentOf(lines[wait]) > indentOf(lines[begin]));
    CHECK(indentOf(lines[shadow]) > indentOf(lines[graph]));
    CHECK(indentOf(lines[acquire]) == indentOf(lines[wait]));
    // Named by their own part, not the whole path.
    CHECK_FALSE(contains(lines[wait], "beginFrame/"));
    // A part whose whole was never timed still appears, by its full name.
    CHECK(lineWith(lines, "endFrame/present:") >= 0);
}

TEST_CASE("the GPU's passes are averaged over every frame read back", "[frame budget]") {
    FrameBudget budget;
    budget.noteGpuFrame({{"frame start", 0.0}, {"shadows", 2.0}, {"world total", 6.0}});
    budget.frameFinished(16.0f);
    budget.noteGpuFrame({{"frame start", 0.0}, {"shadows", 4.0}, {"world total", 10.0}});
    budget.frameFinished(16.0f);

    const auto lines = budget.report("window", 32.0);
    const int head = lineWith(lines, "GPU, 2 frames read back");
    const int shadows = lineWith(lines, "shadows:");
    const int world = lineWith(lines, "world total:");
    REQUIRE(head >= 0);
    REQUIRE(shadows >= 0);
    REQUIRE(world >= 0);
    CHECK(contains(lines[head], "11.00ms/frame avg"));
    CHECK(contains(lines[shadows], "3.00ms avg, p95 4.00, worst 4.00ms"));
    CHECK(contains(lines[world], "8.00ms avg"));
    // In the order the passes ran, not by cost.
    CHECK(shadows < world);
}

TEST_CASE("a new stretch starts from nothing", "[frame budget]") {
    FrameBudget budget;
    budget.noteStage("beginFrame", 5.0f);
    budget.noteGpuFrame({{"shadows", 2.0}});
    budget.frameFinished(60.0f);
    budget.reset();
    CHECK(budget.report("window", 1000.0).empty());

    budget.frameFinished(10.0f);
    const auto lines = budget.report("window", 1000.0);
    REQUIRE(lines.size() == 1);
    CHECK(contains(lines.front(), "over 33.3ms: 0"));
}
