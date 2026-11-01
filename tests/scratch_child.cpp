// The child test_scratch_dir starts: another test process asking for a
// scratch directory, as ctest runs a suite's files at once.
//
//   scratch_child <name>           asks for scratchDir(<name>), writes its
//                                  path on stdout, exits 0
#include "scratch_dir.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const auto dir = wowee::test::scratchDir(argv[1]);
    std::fputs(dir.string().c_str(), stdout);
    return 0;
}
