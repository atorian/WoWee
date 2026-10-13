// How the catalog searches reach a file's contents.
//
// They do not parse .w* files. They re-invoke this executable with the
// format's --info flag and read the JSON back, so the one exporter that
// already knows each format is the only thing that reads it. Four commands
// carried the same four steps to do that, copied byte for byte in three of
// them and reformatted in the fourth.
//
// Every failure on that path is a file silently skipped, which is correct for
// an asset format and invisible when it is caused by something else: seven
// rows of the format table named an --info flag no handler answers, and the
// item catalog was unsearchable by all four commands without a word of
// complaint.
//
// The child is started directly (runChildCapture), not through a shell. It
// used to be a popen of a command line quoted for POSIX sh, which on Windows
// is cmd.exe: a path with '|' or '&' in it ran as a second command there. The
// child here is child_echo, which says back what it was given.
#include <catch_amalgamated.hpp>

#include <string>

#include "cli_subprocess.hpp"

using wowee::editor::cli::runChildCapture;

namespace {

const std::string kChild = CHILD_ECHO_PATH;

// What the child received as its argument.
std::string throughTheChild(const std::string& argument) {
    std::string out;
    const int rc = runChildCapture(kChild, {"print", argument}, out);
    CHECK(rc == 0);
    return out;
}

}  // namespace

TEST_CASE("an argument reaches the child unchanged", "[catalog]") {
    // Data paths are named by whoever made the zone.
    CHECK(throughTheChild("plain") == "plain");
    CHECK(throughTheChild("with space") == "with space");
    CHECK(throughTheChild("Bob's zone") == "Bob's zone");
    CHECK(throughTheChild("quote\"and'both") == "quote\"and'both");
    CHECK(throughTheChild("./Data/terrain/azeroth_32_48.whm") ==
          "./Data/terrain/azeroth_32_48.whm");
    CHECK(throughTheChild("C:\\Data\\zone\\") == "C:\\Data\\zone\\");
    CHECK(throughTheChild("") == "");
}

TEST_CASE("shell metacharacters are data, not syntax", "[catalog]") {
    // A shell would act on any of these; with none in between they are only
    // characters of a filename.
    CHECK(throughTheChild("a;b") == "a;b");
    CHECK(throughTheChild("$(echo no)") == "$(echo no)");
    CHECK(throughTheChild("`echo no`") == "`echo no`");
    CHECK(throughTheChild("a|b") == "a|b");
    CHECK(throughTheChild("a&b") == "a&b");
    CHECK(throughTheChild("a>b") == "a>b");
    CHECK(throughTheChild("%PATH%") == "%PATH%");
    CHECK(throughTheChild("*") == "*");
    CHECK(throughTheChild("~") == "~");
    CHECK(throughTheChild("a\nb") == "a\nb");
}

TEST_CASE("a child that fails is reported as failing", "[catalog]") {
    // Every caller treats a nonzero status as "skip this file", so a child
    // that fails must not look like one that produced nothing to search.
    std::string out;
    CHECK(runChildCapture(kChild, {"exit", "3"}, out) == 3);
    CHECK(out.empty());
}

TEST_CASE("a child that cannot start is -1, not a status", "[catalog]") {
    std::string out;
    CHECK(runChildCapture(kChild + ".missing", {"print", "x"}, out) == -1);
    CHECK(out.empty());
}

TEST_CASE("output longer than one read is captured whole", "[catalog]") {
    // A catalog's JSON runs to hundreds of kilobytes and the capture buffer is
    // four. Truncated output parses as invalid JSON and the file is skipped,
    // which reads as an empty catalog rather than an error.
    std::string out;
    CHECK(runChildCapture(kChild, {"repeat", "200000"}, out) == 0);
    CHECK(out.size() == 200000);
}

TEST_CASE("the child's stderr is not part of what is read", "[catalog]") {
    // The handlers log to stderr; mixed into the JSON it would not parse.
    std::string out;
    CHECK(runChildCapture(kChild, {"stderr", "noise"}, out) == 0);
    CHECK(out.empty());
}
