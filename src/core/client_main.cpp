// What every program that is the client does around its Application: crash
// reporting, the working directory, the log, then initialize, run, shutdown.
//
// It was main() itself. A program of its own - the native loading fixture -
// needed all of it and a frame observer besides, and the only way to give it
// one was to compile the whole client a second time with the fixture inside.
// Now each program's main() calls this, the ordinary client with nothing and
// the fixture with its observer.
#include "core/client_main.hpp"
#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/frame_observer.hpp"
#include "core/logger.hpp"
#include "core/version.hpp"
#include <exception>
#include <csignal>
#include <cstdlib>
#include "core/env.hpp"
#include <cctype>
#include <filesystem>

#include "core/data_paths.hpp"
#include <string>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <ctime>
#endif

// backtrace(3) and friends live in libSystem on macOS and in glibc on Linux, so
// the useful crash report - faulting address, symbolized frames, a copy in the
// crash log - works the same on both. It used to be guarded on __linux__ alone,
// which left a macOS crash with no backtrace and no log at all. Only the X11
// mouse ungrab below is genuinely Linux-specific.
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <libgen.h>
#include <cstdio>
#include <cstring>
// Bionic ships execinfo.h but declares backtrace() only from API 33, and this
// build targets lower. The crash log keeps everything but its stack section on
// Android, where logcat carries a native trace anyway.
#if !defined(__ANDROID__)
#define WOWEE_HAS_BACKTRACE 1
#include <execinfo.h>
#endif
#endif

// Android defines __linux__ and has no X11, so the mouse-ungrab path is gated
// on both. The other Linux branches in this file - /proc/self/exe, the
// backtrace - are correct there and are left alone.
#if defined(__linux__) && !defined(__ANDROID__)
#include <X11/Xlib.h>

// Keep a persistent X11 connection for emergency mouse release in signal handlers.
// XOpenDisplay inside a signal handler is unreliable, so we open it once at startup.
static Display* g_emergencyDisplay = nullptr;

static void releaseMouseGrab() {
    if (g_emergencyDisplay) {
        XUngrabPointer(g_emergencyDisplay, CurrentTime);
        XUngrabKeyboard(g_emergencyDisplay, CurrentTime);
        XFlush(g_emergencyDisplay);
    }
}
#else
static void releaseMouseGrab() {}
#endif

#ifdef WOWEE_HAS_BACKTRACE
static void crashHandlerSigaction(int sig, siginfo_t* info, void* /*ucontext*/) {
    releaseMouseGrab();
    void* frames[64];
    int n = backtrace(frames, 64);
    const char* sigName = (sig == SIGSEGV) ? "SIGSEGV" :
                          (sig == SIGABRT) ? "SIGABRT" :
                          (sig == SIGFPE)  ? "SIGFPE"  : "UNKNOWN";
    void* faultAddr = info ? info->si_addr : nullptr;
    fprintf(stderr, "\n=== CRASH: signal %s (%d) faultAddr=%p ===\n",
            sigName, sig, faultAddr);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    FILE* f = fopen("/tmp/wowee_debug.log", "a");
    if (f) {
        fprintf(f, "\n=== CRASH: signal %s (%d) faultAddr=%p ===\n",
                sigName, sig, faultAddr);
        fflush(f);
        backtrace_symbols_fd(frames, n, fileno(f));
        fclose(f);
    }
    // Re-raise with default handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);
    raise(sig);
}
#else
#ifdef _WIN32
static void reportSignal(int sig);
#endif
static void crashHandler(int sig) {
    releaseMouseGrab();
#ifdef _WIN32
    // abort() - std::terminate, a failed check - raises no structured
    // exception, and the C runtime's own top-level handler can turn an access
    // violation into SIGSEGV before the filter below sees it. Either way the
    // process ended with exit code 3 and nothing to say where.
    if (sig == SIGABRT || sig == SIGSEGV || sig == SIGFPE) reportSignal(sig);
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

// What std::terminate was handed, for the abort report. Copied when it is
// known, because the signal handler that writes the report must not allocate.
static char g_terminateReason[512] = "";

/// An exception nothing caught - thrown from a destructor, or out of a thread
/// - ends in std::terminate, which used to say nothing about which one.
[[noreturn]] static void terminateWithReason() {
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            std::snprintf(g_terminateReason, sizeof(g_terminateReason),
                          "terminate: uncaught exception: %s", e.what());
        } catch (...) {
            std::snprintf(g_terminateReason, sizeof(g_terminateReason),
                          "terminate: uncaught exception of a non-standard type");
        }
    } else {
        std::snprintf(g_terminateReason, sizeof(g_terminateReason),
                      "terminate: called without an active exception");
    }
    std::fprintf(stderr, "\n%s\n", g_terminateReason);
    std::fflush(stderr);
    std::abort();
}

#ifdef _WIN32
// A crash on Windows, from any thread.
//
// signal(SIGSEGV) only ever saw the main thread's faults, and only to raise
// them again. One on a worker - the M2 animation, the particle and command
// recording threads - went to Windows Error Reporting, and with that off the
// process simply vanished: the log stopped at "ENTERING WORLD" with nothing
// after it. This says what happened and where, as module+offset frames that
// addr2line turns into source lines (the build carries -g), on stderr and in
// logs/crash-<time>.log. Nothing here allocates: the heap may be the thing
// that broke.
static void writeCrashFrame(HANDLE out, FILE* file, const char* text) {
    DWORD written = 0;
    if (out != INVALID_HANDLE_VALUE) WriteFile(out, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    if (file) fputs(text, file);
}

static void describeAddress(void* address, char* buffer, size_t size) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) && module) {
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        snprintf(buffer, size, "%p  %s+0x%llx\n", address, name ? name + 1 : path,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                 reinterpret_cast<uintptr_t>(module)));
    } else {
        snprintf(buffer, size, "%p  ?\n", address);
    }
}

static FILE* openCrashLog() {
    char fileName[64];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(fileName, sizeof(fileName), "logs/crash-%Y%m%d-%H%M%S.log", &local);
    return std::fopen(fileName, "w");
}

// The stack as it stands in the handler: below its own frames and the
// dispatcher's are the failing ones.
static void writeCrashStack(HANDLE err, FILE* file) {
    char line[512];
    void* frames[64];
    const USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    writeCrashFrame(err, file, "stack:\n");
    for (USHORT i = 0; i < count; ++i) {
        describeAddress(frames[i], line, sizeof(line));
        writeCrashFrame(err, file, line);
    }
}

static void reportSignal(int sig) {
    FILE* file = openCrashLog();
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    char line[600];
    const char* name = sig == SIGABRT ? "abort" : sig == SIGSEGV ? "SIGSEGV" : "SIGFPE";
    snprintf(line, sizeof(line), "\n=== CRASH: %s at thread %lu ===\n%s%s", name, GetCurrentThreadId(),
             g_terminateReason, g_terminateReason[0] ? "\n" : "");
    writeCrashFrame(err, file, line);
    writeCrashStack(err, file);
    if (file) std::fclose(file);
}

static LONG WINAPI windowsCrashFilter(EXCEPTION_POINTERS* info) {
    releaseMouseGrab();
    char line[512];
    FILE* file = openCrashLog();
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);

    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    snprintf(line, sizeof(line), "\n=== CRASH: exception 0x%08lx at thread %lu ===\n",
             record ? record->ExceptionCode : 0ul, GetCurrentThreadId());
    writeCrashFrame(err, file, line);
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        snprintf(line, sizeof(line), "%s of address 0x%llx\n",
                 record->ExceptionInformation[0] == 0 ? "read" :
                 record->ExceptionInformation[0] == 1 ? "write" : "execute",
                 static_cast<unsigned long long>(record->ExceptionInformation[1]));
        writeCrashFrame(err, file, line);
    }
    if (record) {
        writeCrashFrame(err, file, "faulting instruction: ");
        describeAddress(record->ExceptionAddress, line, sizeof(line));
        writeCrashFrame(err, file, line);
    }
    writeCrashStack(err, file);
    if (file) std::fclose(file);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static wowee::core::LogLevel readLogLevelFromEnv() {
    const char* raw = std::getenv("WOWEE_LOG_LEVEL");
    if (!raw || !*raw) return wowee::core::LogLevel::WARNING;
    std::string level(raw);
    for (char& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (level == "debug") return wowee::core::LogLevel::DEBUG;
    if (level == "info") return wowee::core::LogLevel::INFO;
    if (level == "warn" || level == "warning") return wowee::core::LogLevel::WARNING;
    if (level == "error") return wowee::core::kLogLevelError;
    if (level == "fatal") return wowee::core::LogLevel::FATAL;
    return wowee::core::LogLevel::WARNING;
}

/// Point the client at the per-user data directory, if an extraction is there.
///
/// Only when nobody has said otherwise, and only when that directory actually
/// holds an extraction - so a first run with nothing installed still falls
/// through to Data/ beside the executable, which is what a development tree
/// and a portable install both want.
///
/// This was macOS only, which left the asset manager and the client with no
/// agreed destination on Linux or Windows at all: the manager wrote wherever
/// the terminal was and the client looked beside itself.
static void selectUserDataPath() {
    if (std::getenv("WOW_DATA_PATH")) return;

    const std::filesystem::path dataRoot = wowee::core::userDataRoot();
    if (!wowee::core::holdsExtraction(dataRoot)) return;

    wowee::core::setEnvVar("WOW_DATA_PATH", dataRoot.string().c_str(), /*overwrite=*/false);
}

namespace wowee::core {

int runClient(const ClientProgram& program) {
#ifdef __ANDROID__
    // Everything after this opens its files relative to the working directory,
    // which on Android is not a directory that holds any of them.
    wowee::core::enterResourceRoot();
#endif
#ifndef _WIN32
    // Writing to a socket the server has already closed raises SIGPIPE, whose
    // default action is to terminate - the client would vanish mid-frame with
    // no log line and no crash report, because SIGPIPE is not one of the
    // signals handled below. Ignoring it makes send() answer EPIPE instead,
    // which the send paths already handle: they log the failure and stop
    // writing, and the recv side sees the closed connection and disconnects.
    //
    // Done process-wide rather than per-socket: the flag that suppresses this
    // at the call site is spelled differently on each platform (MSG_NOSIGNAL
    // on Linux, the SO_NOSIGPIPE socket option on macOS/BSD), and nothing here
    // wants SIGPIPE for anything.
    std::signal(SIGPIPE, SIG_IGN);
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    g_emergencyDisplay = XOpenDisplay(nullptr);
#endif
#ifdef WOWEE_HAS_BACKTRACE
    // Use sigaction for SIGSEGV/SIGABRT/SIGFPE to get si_addr (faulting address)
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crashHandlerSigaction;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGABRT, &sa, nullptr);
        sigaction(SIGFPE,  &sa, nullptr);
    }
    std::signal(SIGTERM, [](int) { std::_Exit(1); });
    std::signal(SIGINT,  [](int) { std::_Exit(1); });
#elif defined(__ANDROID__)
    // Android already has a crash reporter, and it is a far better one than
    // this: debuggerd writes a symbolised tombstone and the abort message to
    // logcat. Ours writes a backtrace to stderr, which on Android goes nowhere,
    // and installing it costs the tombstone. So only the two exit signals here.
    std::signal(SIGTERM, crashHandler);
    std::signal(SIGINT,  crashHandler);
#else
    std::signal(SIGSEGV, crashHandler);
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGFPE,  crashHandler);
    std::signal(SIGTERM, crashHandler);
    std::signal(SIGINT,  crashHandler);
#endif
#ifdef _WIN32
    SetUnhandledExceptionFilter(windowsCrashFilter);
#endif
    std::set_terminate(terminateWithReason);
    // Change working directory so relative asset paths resolve from any launch
    // location. A signed macOS bundle keeps data in Contents/Resources because
    // Contents/MacOS may contain code only.
#ifdef __APPLE__
    {
        uint32_t bufSize = 0;
        _NSGetExecutablePath(nullptr, &bufSize);
        std::string exePath(bufSize, '\0');
        _NSGetExecutablePath(exePath.data(), &bufSize);
        const std::filesystem::path executableDir =
            std::filesystem::path(exePath.c_str()).parent_path();
        const std::filesystem::path resourceDir =
            executableDir.parent_path() / "Resources";
        const std::filesystem::path runtimeDir =
            std::filesystem::is_directory(resourceDir / "assets")
                ? resourceDir
                : executableDir;
        if (chdir(runtimeDir.c_str()) != 0) {}
    }
#elif defined(__linux__)
    {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) { buf[len] = '\0'; if (chdir(dirname(buf)) != 0) {} }
    }
#endif

    // After the working directory is settled, and on every platform: this used
    // to sit inside the macOS branch above, so Linux and Windows went on
    // looking beside the executable for assets the asset manager had written
    // to their own per-user directory.
    selectUserDataPath();

    try {
        wowee::core::Logger::getInstance().setLogLevel(readLogLevelFromEnv());
        LOG_INFO("=== Wowee Native Client ===");
        // At warning level, with the platform, because a log sent in after a
        // crash is filtered to warnings and errors. Which build produced it is
        // the first question asked of any report and the log did not answer it:
        // a device loss on a version predating the fix for that very loss reads
        // exactly like a new one.
#if defined(_WIN32)
        constexpr const char* kPlatform = "windows";
#elif defined(__ANDROID__)
        constexpr const char* kPlatform = "android";
#elif defined(__APPLE__)
        constexpr const char* kPlatform = "macos";
#elif defined(__linux__)
        constexpr const char* kPlatform = "linux";
#else
        constexpr const char* kPlatform = "unknown-platform";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        constexpr const char* kArch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        constexpr const char* kArch = "x86-64";
#else
        constexpr const char* kArch = "unknown-arch";
#endif
        LOG_WARNING("Wowee ", wowee::core::kVersionString, " ", kPlatform, " ", kArch);
        LOG_INFO("Starting application...");

        // Seed portable config from the per-user location on first portable launch.
        wowee::core::migratePortableConfigIfNeeded();

        wowee::core::Application app;

        if (!app.initialize()) {
            LOG_FATAL("Failed to initialize application");
            return 1;
        }

        if (program.observeFrames) {
            auto observer = program.observeFrames(app);
            if (!observer) {
                LOG_FATAL("This program watches the client's frames and could not start "
                          "its observer; not running the client without it");
                app.shutdown();
                return 1;
            }
            app.observeFrames(std::move(observer));
        }

        app.run();
        app.shutdown();

        LOG_INFO("Application exited successfully");
#if defined(__linux__) && !defined(__ANDROID__)
        if (g_emergencyDisplay) { XCloseDisplay(g_emergencyDisplay); g_emergencyDisplay = nullptr; }
#endif
        return 0;
    }
    catch (const std::exception& e) {
        releaseMouseGrab();
        LOG_FATAL("Unhandled exception: ", e.what());
        return 1;
    }
    catch (...) {
        releaseMouseGrab();
        LOG_FATAL("Unknown exception occurred");
        return 1;
    }
}

} // namespace wowee::core
