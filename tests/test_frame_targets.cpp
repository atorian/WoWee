// A frame is recorded only against a complete set of frame targets. When one
// of them - the depth buffer, say - cannot be made, nothing records against
// the set until it is made again, and the failure is said once per stretch
// with the result, the size asked for and the driver's budget.
#include <catch_amalgamated.hpp>

#include "rendering/frame_targets.hpp"

#include <optional>
#include <string>

using wowee::rendering::FrameTargets;
using wowee::rendering::GpuHeapReading;

namespace {

constexpr uint64_t kMiB = 1024ull * 1024;

std::optional<GpuHeapReading> fullHeap() {
    GpuHeapReading heap;
    heap.deviceLocal = true;
    heap.sizeBytes = 12288 * kMiB;
    heap.budgetBytes = 11000 * kMiB;
    heap.usageBytes = 10990 * kMiB;
    heap.fromDriver = true;
    return heap;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("nothing is recorded against frame targets that could not be made", "[frame_targets]") {
    FrameTargets targets;
    CHECK_FALSE(targets.usable());  // before the first set exists

    targets.rebuilding();
    CHECK_FALSE(targets.usable());
    (void)targets.made();
    CHECK(targets.usable());

    // The window grows after login and the depth buffer no longer fits.
    targets.rebuilding();
    const auto said = targets.failed("depth buffer", VK_ERROR_OUT_OF_DEVICE_MEMORY, 2560, 1440, fullHeap());
    CHECK_FALSE(targets.usable());
    REQUIRE(said);
    CHECK(contains(*said, "depth buffer"));
    CHECK(contains(*said, "2560x1440"));
    CHECK(contains(*said, "VK_ERROR_OUT_OF_DEVICE_MEMORY"));
    CHECK(contains(*said, "10990 MB in use of a 11000 MB budget"));

    // Rebuilt every frame and failing every time: said once.
    for (int frame = 0; frame < 5; ++frame) {
        targets.rebuilding();
        CHECK_FALSE(targets.failed("depth buffer", VK_ERROR_OUT_OF_DEVICE_MEMORY, 2560, 1440, fullHeap()));
        CHECK_FALSE(targets.usable());
    }

    // Room again: frames are drawn, and the recovery is said.
    targets.rebuilding();
    const auto recovered = targets.made();
    CHECK(targets.usable());
    REQUIRE(recovered);
    CHECK(contains(*recovered, "6 failed attempt"));

    // A later failure is a new stretch and is said again.
    targets.rebuilding();
    CHECK(targets.failed("MSAA colour image", VK_ERROR_OUT_OF_DEVICE_MEMORY, 2560, 1440, std::nullopt));
}
