// A test's scratch directory, as other test processes running at the same
// time see it.
//
// ctest runs each file of a suite as a process of the same executable, many
// at once, and every GameHarness and ClientHarness asks for the same names.
// When the name was the directory, each process emptied the others' on its
// way in: their settings and saved variables went, or creating the directory
// failed between another's removal and its own creation. The child here is
// scratch_child, a second process asking for the same name.
#include <catch_amalgamated.hpp>

#include "cli_subprocess.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>

using wowee::editor::cli::runChildCapture;
using wowee::test::scratchDir;
using wowee::test::writeFile;

namespace {

const std::string kChild = SCRATCH_CHILD_PATH;

}  // namespace

TEST_CASE("another process asking for the same scratch leaves this one's alone", "[scratch]") {
    const auto mine = scratchDir("wowee_scratch_dir_shared");
    writeFile(mine / "kept", {1, 2, 3});

    std::string theirs;
    REQUIRE(runChildCapture(kChild, {"wowee_scratch_dir_shared"}, theirs) == 0);

    CHECK(std::filesystem::exists(mine / "kept"));
}

TEST_CASE("a process's scratch is gone once it has exited", "[scratch]") {
    std::string theirs;
    REQUIRE(runChildCapture(kChild, {"wowee_scratch_dir_left"}, theirs) == 0);
    REQUIRE_FALSE(theirs.empty());
    CHECK_FALSE(std::filesystem::exists(theirs));
}
