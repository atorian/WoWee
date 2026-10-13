// The child test_catalog_subprocess starts. It answers in the ways a format's
// --info handler can, so the launcher is tested without a shell or a handler:
//
//   child_echo print <text>        <text> on stdout, exit 0
//   child_echo exit <n>            nothing, exit <n>
//   child_echo repeat <n>          'x' n times on stdout
//   child_echo stderr <text>       <text> on stderr only
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // Bytes as given: text mode would turn a newline into CR LF.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 3) return 2;
    const char* mode = argv[1];
    if (std::strcmp(mode, "print") == 0) {
        std::fputs(argv[2], stdout);
        return 0;
    }
    if (std::strcmp(mode, "exit") == 0) return std::atoi(argv[2]);
    if (std::strcmp(mode, "repeat") == 0) {
        for (int i = std::atoi(argv[2]); i > 0; --i) std::fputc('x', stdout);
        return 0;
    }
    if (std::strcmp(mode, "stderr") == 0) {
        std::fputs(argv[2], stderr);
        return 0;
    }
    return 2;
}
