// Scenes drawn by scene_render, each against the picture accepted for it.
//
// What runs on the GPU is tested on the GPU (AGENTS.md): the scene is drawn
// by the client's renderer and the picture compared with its approved one in
// tests/approvals. The renderer is deterministic on one machine, so the
// comparison is exact. The drawn picture stays in the build's
// scratch/scene_references; when it fails, look at it, and if it is right,
// copy it over tests/approvals/test_scene_references.<case>.approved.png.
//
// Skipped without a Vulkan device or the game data (WOW_DATA_PATH, which the
// build points at WOWEE_GAME_DATA_DIR).
#define APPROVALS_CATCH2_V3
#include <catch_amalgamated.hpp>
#include "ApprovalTests.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

/// `scene` (in tools/scene_render/scenes) drawn by scene_render into
/// `picture`. Skips the test where it cannot be drawn at all.
void draw(const std::string& scene, const fs::path& picture) {
    const fs::path renderer = WOWEE_SCENE_RENDER;
    const fs::path source = fs::path(WOWEE_SOURCE_DIR) / "tools" / "scene_render" / "scenes" / scene;
    fs::create_directories(picture.parent_path());
    // In its own directory: it loads its shaders and assets from there.
    const std::string command = "cd \"" + renderer.parent_path().string() + "\" && \"" + renderer.string() +
                                "\" \"" + source.string() + "\" \"" + picture.string() + "\" 2>&1";
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
    const int status = pclose(pipe);
    if (output.find("no Vulkan device") != std::string::npos ||
        output.find("no assets under") != std::string::npos) {
        SKIP("scene_render cannot draw here: no Vulkan device or no game data");
    }
    INFO(output.substr(output.size() > 4000 ? output.size() - 4000 : 0));
    REQUIRE(status == 0);
    REQUIRE(fs::exists(picture));
}

/// The picture checked against tests/approvals, quietly: no diff tool is
/// opened from a test run.
void verifyPicture(const fs::path& picture) {
    auto directory = ApprovalTests::Approvals::useApprovalsSubdirectory("approvals");
    ApprovalTests::Approvals::verifyExistingFile(
        picture.string(), ApprovalTests::Options().withReporter(ApprovalTests::QuietReporter()));
}

}  // namespace

TEST_CASE("water wake", "[scene][water]") {
    // A calm sea, seen low into the sunset: from above, or with the wind's
    // ripples on it, the wake's waves do not show. Three bodies part in the
    // water: one at rest sits in a hollow and raises no waves, the two on the
    // move leave Kelvin's wedge and white water, the faster the longer.
    const fs::path picture = fs::path(WOWEE_SCRATCH_DIR) / "scene_references" / "water_wake.png";
    draw("water_wake.json", picture);
    verifyPicture(picture);
}

TEST_CASE("shadows", "[scene][shadow]") {
    // A lamppost and a farmhouse on open ground in the late afternoon: each
    // casts its shadow on the ground, which keeps 70% of the light and is
    // soft at its edge, the lamppost's sharp to its lantern.
    const fs::path picture = fs::path(WOWEE_SCRATCH_DIR) / "scene_references" / "shadows.png";
    draw("shadows.json", picture);
    verifyPicture(picture);
}

TEST_CASE("sphere map", "[scene][m2]") {
    // Models whose shine is a sphere environment map (texture coordinate set
    // -1): a mage-hunter crystal and Murloc's great pearl, the pearl's whole
    // surface the map.
    const fs::path picture = fs::path(WOWEE_SCRATCH_DIR) / "scene_references" / "sphere_map.png";
    draw("sphere_map.json", picture);
    verifyPicture(picture);
}
