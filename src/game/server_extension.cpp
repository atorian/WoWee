#include "game/server_extension.hpp"
#include "game/server_extension_wire.hpp"
#include "core/https_get.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <array>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <fstream>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace wowee::game {
namespace fs = std::filesystem;
namespace wire = extension_wire;
namespace {
constexpr uint64_t maxArtifact = 64ull * 1024 * 1024;
fs::path utf8Path(const std::string& value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::string digest(const std::vector<uint8_t>& bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned size = 0;
    if (!EVP_Digest(bytes.data(), bytes.size(), hash.data(), &size, EVP_sha256(), nullptr))
        throw std::runtime_error("Cannot compute library checksum");
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < size; ++i) { out += hex[hash[i] >> 4]; out += hex[hash[i] & 15]; }
    return out;
}
std::vector<uint8_t> readFile(const fs::path& path, uint64_t cap) {
    const auto size = fs::file_size(path);
    if (!size || size > cap) throw std::runtime_error("Invalid file size: " + path.string());
    std::vector<uint8_t> data(static_cast<size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(data.data()), data.size()))
        throw std::runtime_error("Cannot read " + path.string());
    return data;
}
fs::path artifact(const nlohmann::json& item, const fs::path& directory) {
    const auto expected = item.at("sha256").get<std::string>();
    if (expected.size() != 64 || expected.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Artifact requires a lowercase SHA256 checksum");
    const auto local = item.value("path", std::string());
    if (local.find('\0') != std::string::npos) throw std::runtime_error("Invalid artifact path");
    if (!local.empty()) {
        const fs::path configured = utf8Path(local);
        const auto path = fs::absolute(configured.is_absolute() ? configured : directory / configured);
        if (fs::exists(path)) {
            if (digest(readFile(path, maxArtifact)) != expected)
                throw std::runtime_error("Library checksum mismatch: " + path.string());
            return path;
        }
    }
    const auto cache = directory / "cache" / (expected + ".dll");
    if (fs::exists(cache) && digest(readFile(cache, maxArtifact)) == expected) return fs::absolute(cache);
    const auto url = item.value("url", std::string());
    if (url.rfind("https://", 0) != 0) throw std::runtime_error("Artifact missing; configure an HTTPS URL or local path");
    const auto slash = url.find('/', 8);
    if (slash == std::string::npos) throw std::runtime_error("Artifact URL needs an absolute path");
    const auto host = url.substr(8, slash - 8);
    if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos
        || url.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        throw std::runtime_error("Invalid artifact URL");
    auto response = core::httpsGet(host, url.substr(slash), "WoWee-Extension/1", 8, maxArtifact);
    if (!response.ok || response.status != 200)
        throw std::runtime_error("Library download failed: " + (response.error.empty() ? std::to_string(response.status) : response.error));
    const std::vector<uint8_t> data(response.body.begin(), response.body.end());
    if (data.empty() || digest(data) != expected) throw std::runtime_error("Downloaded library checksum mismatch");
    fs::create_directories(cache.parent_path());
    const auto pending = fs::path(cache.string() + ".pending");
    {
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data.data()), data.size());
        if (!stream) throw std::runtime_error("Cannot save downloaded library");
    }
    // Keep the previous verified version untouched until the new hash passed.
    std::error_code ec; fs::remove(cache, ec); fs::rename(pending, cache);
    return fs::absolute(cache);
}
#ifdef _WIN32
std::wstring quote(const std::wstring& argument) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const auto c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); out += L'"';
    return out;
}
#endif
}
std::string serverExtensionKey(std::string host, uint16_t port) {
    const auto first = host.find_first_not_of(" \t\r\n");
    const auto last = host.find_last_not_of(" \t\r\n");
    host = first == std::string::npos ? "" : host.substr(first, last - first + 1);
    for (auto& c : host) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const auto identity = host + ":" + std::to_string(port);
    const auto hash = digest({identity.begin(), identity.end()}).substr(0, 16);
    std::string label;
    for (const auto c : host) {
        if (label.size() == 32) break;
        label += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    if (label.empty()) label = "server";
    return label + "-" + std::to_string(port) + "-" + hash;
}
struct ServerExtension::Impl {
    std::string error, client;
    std::unique_ptr<ServerProtocolAdapter> protocol;
    std::unique_ptr<ServerProtocolTrace> trace;
    std::mutex requestMutex;
    struct RuntimeRange { std::string module; uint32_t address; uint64_t size; };
    std::vector<RuntimeRange> runtimeRanges;
#ifdef _WIN32
    HANDLE process = nullptr, job = nullptr, input = nullptr, output = nullptr;
#else
    pid_t process = -1;
    int input = -1, output = -1;
#endif
    ~Impl() { close(); }
    bool running() const {
#ifdef _WIN32
        return process != nullptr;
#else
        return process > 0;
#endif
    }
    void close() {
#ifdef _WIN32
        // EOF lets a cooperative adapter run stop(); a hung/crashed host is
        // still bounded and its whole job is discarded.
        if (input) { CloseHandle(input); input = nullptr; }
        if (process) {
            if (WaitForSingleObject(process, 100) != WAIT_OBJECT_0) TerminateProcess(process, 0);
            WaitForSingleObject(process, 1000);
            CloseHandle(process);
        }
        if (job) CloseHandle(job);
        if (output) CloseHandle(output);
        process = job = input = output = nullptr;
#else
        if (input >= 0) ::close(input);
        if (output >= 0) ::close(output);
        if (process > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            bool exited = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto result = waitpid(process, nullptr, WNOHANG);
                if (result == process || (result < 0 && errno == ECHILD)) { exited = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            kill(-process, SIGKILL);
            if (!exited) { kill(process, SIGKILL); while (waitpid(process, nullptr, 0) < 0 && errno == EINTR) {} }
        }
        process = -1; input = output = -1;
#endif
    }
    void spawn(const fs::path& host, const fs::path& adapter, const fs::path& directory) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childIn = nullptr, childOut = nullptr;
        if (!CreatePipe(&childIn, &input, &security, 32768) || !CreatePipe(&output, &childOut, &security, 32768)) {
            if (childIn) CloseHandle(childIn);
            if (childOut) CloseHandle(childOut);
            throw std::runtime_error("Cannot create extension pipes");
        }
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        HANDLE nullLog = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &security, OPEN_EXISTING, 0, nullptr);
        // Only these three handles cross the process boundary.
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<uint8_t> attributes(attributeSize);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childIn; startup.StartupInfo.hStdOutput = childOut; startup.StartupInfo.hStdError = nullLog;
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        const HANDLE inherited[] = {childIn, childOut, nullLog};
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize)
            || !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           const_cast<HANDLE*>(inherited), sizeof(inherited), nullptr, nullptr)) {
            CloseHandle(childIn); CloseHandle(childOut); CloseHandle(nullLog);
            throw std::runtime_error("Cannot isolate extension handles");
        }
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            CloseHandle(childIn); CloseHandle(childOut); CloseHandle(nullLog);
            throw std::runtime_error("Cannot supervise extension host");
        }
        auto command = quote(host.wstring()) + L" " + quote(adapter.wstring());
        PROCESS_INFORMATION child{};
        const bool ok = CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &child);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(childIn); CloseHandle(childOut); CloseHandle(nullLog);
        if (!ok) throw std::runtime_error("Cannot launch extension host (Windows error " + std::to_string(GetLastError()) + ")");
        process = child.hProcess;
        if (!AssignProcessToJobObject(job, process)) {
            CloseHandle(child.hThread); throw std::runtime_error("Cannot supervise extension process");
        }
        ResumeThread(child.hThread); CloseHandle(child.hThread);
#else
        int toChild[2], fromChild[2];
        if (pipe(toChild) != 0) throw std::runtime_error("Cannot create extension input pipe");
        if (pipe(fromChild) != 0) { ::close(toChild[0]); ::close(toChild[1]); throw std::runtime_error("Cannot create extension output pipe"); }
        const auto executable = host.string(), argument = adapter.string(), cwd = directory.string();
        const long descriptorLimit = sysconf(_SC_OPEN_MAX);
        process = fork();
        if (process == 0) {
            setpgid(0, 0);
            dup2(toChild[0], STDIN_FILENO); dup2(fromChild[1], STDOUT_FILENO);
            ::close(toChild[0]); ::close(toChild[1]); ::close(fromChild[0]); ::close(fromChild[1]);
#ifdef __linux__
            if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
                for (long fd = 3; fd < descriptorLimit; ++fd) ::close(static_cast<int>(fd));
#else
            for (long fd = 3; fd < descriptorLimit; ++fd) ::close(static_cast<int>(fd));
#endif
            const int log = open("/dev/null", O_WRONLY); if (log >= 0) { dup2(log, STDERR_FILENO); ::close(log); }
            if (chdir(cwd.c_str()) != 0) _exit(126);
            execl(executable.c_str(), executable.c_str(), argument.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(toChild[0]); ::close(fromChild[1]);
        input = toChild[1]; output = fromChild[0];
        if (process > 0) setpgid(process, process);
        if (process < 0) throw std::runtime_error("Cannot launch extension host");
        fcntl(input, F_SETFL, O_NONBLOCK); fcntl(output, F_SETFL, O_NONBLOCK);
#endif
    }
    std::vector<uint8_t> exchange(uint32_t operation, const std::vector<uint8_t>& payload) {
        const auto request = wire::frame(operation, payload);
#ifdef _WIN32
        DWORD written = 0;
        // Messages fit in the pipe buffer even if the host is unresponsive.
        if (!WriteFile(input, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) || written != request.size())
            throw std::runtime_error("Extension host stopped accepting requests");
#else
        // Block SIGPIPE only on this thread, and consume our own EPIPE signal.
        sigset_t mask, old; sigemptyset(&mask); sigaddset(&mask, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &mask, &old);
        const auto written = write(input, request.data(), request.size());
        if (written < 0 && errno == EPIPE && !sigismember(&old, SIGPIPE)) {
            timespec instant{}; sigtimedwait(&mask, nullptr, &instant);
        }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        if (written != static_cast<ssize_t>(request.size())) throw std::runtime_error("Extension host stopped accepting requests");
#endif
        std::vector<uint8_t> response;
        uint32_t expected = 12;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Extension host timed out");
            uint8_t buffer[wire::maxPayload + 12];
#ifdef _WIN32
            DWORD available = 0, received = 0;
            if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) throw std::runtime_error("Extension host exited");
            if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            if (!ReadFile(output, buffer, std::min<DWORD>(available, expected - response.size()), &received, nullptr))
                throw std::runtime_error("Extension host exited");
#else
            pollfd item{output, POLLIN, 0};
            if (poll(&item, 1, 10) < 0) { if (errno == EINTR) continue; throw std::runtime_error("Extension pipe failed"); }
            const auto received = read(output, buffer, expected - response.size());
            if (received < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (received <= 0) throw std::runtime_error("Extension host exited");
#endif
            response.insert(response.end(), buffer, buffer + received);
            if (response.size() == 12 && expected == 12) {
                wire::Reader h(response);
                if (h.integer() != wire::magic || h.integer() != operation) throw std::runtime_error("Invalid extension reply");
                const auto size = h.integer();
                if (size < 8 || size > wire::maxPayload) throw std::runtime_error("Invalid extension reply size");
                expected += size;
            } else if (response.size() == expected) return {response.begin() + 12, response.end()};
        }
    }
};
ServerExtension::ServerExtension() : impl_(std::make_unique<Impl>()) {}
ServerExtension::~ServerExtension() = default;
void ServerExtension::reset() { impl_ = std::make_unique<Impl>(); }
bool ServerExtension::active() const { return impl_->error.empty() && (impl_->running() || bool(impl_->protocol)); }
const std::string& ServerExtension::error() const { return impl_->error; }
const std::string& ServerExtension::clientPath() const { return impl_->client; }
bool ServerExtension::begin(const fs::path& root, const std::string& host, uint16_t port, uint32_t build, const fs::path& defaultHost, const ProtocolEvents& events) {
    reset();
    try {
        const auto directory = fs::absolute(root / serverExtensionKey(host, port));
        const auto path = directory / "profile.json";
        if (!fs::exists(path)) return true;
        const auto bytes = readFile(path, 65536);
        const auto profile = nlohmann::json::parse(bytes);
        if (profile.at("version").get<int>() != 1) throw std::runtime_error("Unsupported server extension profile version");
        if (profile.at("build").get<uint32_t>() != build) throw std::runtime_error("Server extension profile has a different client build");
        const auto provider = profile.at("provider").get<std::string>();
        if (profile.contains("runtime_ranges")) {
            const auto& ranges = profile.at("runtime_ranges");
            if (!ranges.is_array() || ranges.size() > 64) throw std::runtime_error("Invalid runtime memory ranges");
            for (const auto& range : ranges) {
                const auto address = range.at("address").get<int64_t>();
                const auto size = range.at("size").get<int64_t>();
                if (address < 0 || size <= 0 || size > static_cast<int64_t>(maxArtifact)
                    || static_cast<uint64_t>(address) + static_cast<uint64_t>(size) > 0x100000000ull)
                    throw std::runtime_error("Invalid runtime memory range");
                auto module = range.value("module", std::string());
                for (auto& c : module) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (module == "wow.exe") module.clear();
                impl_->runtimeRanges.push_back({module, static_cast<uint32_t>(address), static_cast<uint64_t>(size)});
            }
        }
        if (profile.contains("protocol")) {
            if (profile.contains("adapter")) throw std::runtime_error("Choose a built-in protocol or a native adapter");
            impl_->protocol = makeServerProtocol(profile.at("protocol").get<std::string>(), {host, port, build, events});
            if (profile.value("trace", false)) impl_->trace = std::make_unique<ServerProtocolTrace>(directory / "trace");
            if (profile.contains("client")) {
                const auto value = artifact(profile.at("client"), directory).u8string();
                impl_->client.assign(value.begin(), value.end());
            }
            return true;
        }
        if (!profile.contains("adapter"))
            throw std::runtime_error(provider + ": a compatible WoWee adapter is required. The original WoW DLL cannot run directly in WoWee.");
        // Validate/download adapter first: never fetch a library we cannot use.
        const auto adapter = artifact(profile.at("adapter"), directory);
        const auto library = artifact(profile.at("library"), directory);
        if (profile.contains("client")) {
            const auto value = artifact(profile.at("client"), directory).u8string();
            impl_->client.assign(value.begin(), value.end());
        }
        fs::path executable = defaultHost;
        if (profile.contains("host")) {
            executable = utf8Path(profile.at("host").get<std::string>());
            if (executable.is_relative()) executable = directory / executable;
        }
        executable = fs::absolute(executable);
        if (!fs::is_regular_file(executable)) throw std::runtime_error("Extension host executable is missing");
        const auto options = profile.value("options", nlohmann::json::object()).dump();
        std::vector<uint8_t> request;
        wire::string(request, host); wire::integer(request, port); wire::integer(request, build);
        const auto libraryUtf8 = library.u8string();
        wire::string(request, {libraryUtf8.begin(), libraryUtf8.end()}); wire::string(request, impl_->client); wire::string(request, options);
        impl_->spawn(executable, adapter, directory);
        const auto reply = impl_->exchange(wire::Start, request);
        wire::Reader r(reply); const auto status = r.integer(); const auto why = r.string(); r.end();
        if (status != 1) throw std::runtime_error(why.empty() ? "Server extension initialization failed" : why);
        return true;
    } catch (const std::exception& e) { impl_->close(); impl_->protocol.reset(); impl_->error = e.what(); return false; }
}
ServerExtension::ReadResult ServerExtension::readMemory(const std::string& module, uint32_t address, uint8_t length, uint8_t* output) {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    if (!impl_->error.empty()) return ReadResult::Error;
    if (!active()) return ReadResult::Unhandled;
    try {
        if (impl_->protocol) {
            const auto result = impl_->protocol->memory(module, address, length, output);
            if (result == ProtocolResult::Error) throw std::runtime_error(impl_->protocol->error());
            if (result == ProtocolResult::Handled) return ReadResult::Handled;
            auto normalized = module;
            for (auto& c : normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (normalized == "wow.exe") normalized.clear();
            for (const auto& range : impl_->runtimeRanges)
                if (length && range.module == normalized && uint64_t(address) + length > range.address
                    && uint64_t(range.address) + range.size > address)
                    throw std::runtime_error("Protocol did not provide required runtime memory");
            return ReadResult::Unhandled;
        }
        std::vector<uint8_t> request; wire::string(request, module); wire::integer(request, address); wire::integer(request, length);
        const auto reply = impl_->exchange(wire::Read, request);
        wire::Reader r(reply); const auto status = r.integer(); const auto why = r.string();
        // Wire prefix is [status][error length][error bytes], followed by data.
        const auto prefix = 8 + why.size();
        if (status == 2) throw std::runtime_error(why.empty() ? "Server extension read failed" : why);
        if (status == 0) {
            r.end();
            auto normalized = module;
            for (auto& c : normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (normalized == "wow.exe") normalized.clear();
            for (const auto& range : impl_->runtimeRanges) {
                if (range.module == normalized && static_cast<uint64_t>(address) + length > range.address
                    && static_cast<uint64_t>(range.address) + range.size > address)
                    throw std::runtime_error("Adapter did not provide required runtime memory");
            }
            return ReadResult::Unhandled;
        }
        if (status != 1 || reply.size() != prefix + length) throw std::runtime_error("Invalid runtime memory reply");
        if (length) std::copy(reply.begin() + prefix, reply.end(), output);
        return ReadResult::Handled;
    } catch (const std::exception& e) { impl_->close(); impl_->error = e.what(); return ReadResult::Error; }
}

ProtocolResult ServerExtension::packet(uint16_t opcode, const std::vector<uint8_t>& body, const ProtocolSend& send) {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    if (!impl_->protocol) return ProtocolResult::Unhandled;
    if (!impl_->error.empty()) return ProtocolResult::Error;
    try {
        if (impl_->trace) impl_->trace->record(2, opcode, body);
        const auto result = impl_->protocol->packet(opcode, body, send);
        if (result == ProtocolResult::Error) impl_->error = impl_->protocol->error();
        return result;
    } catch (const std::exception& e) { impl_->error = e.what(); return ProtocolResult::Error; }
}
bool ServerExtension::takeEarlyCharacterListRetryRequest() {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    return impl_->protocol && impl_->error.empty() && impl_->protocol->takeEarlyCharacterListRetryRequest();
}
bool ServerExtension::takeCharacterListRetryRequest() {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    return impl_->protocol && impl_->error.empty() && impl_->protocol->takeCharacterListRetryRequest();
}
void ServerExtension::observeWarden(const std::vector<uint8_t>& plaintext) {
    std::lock_guard<std::mutex> lock(impl_->requestMutex);
    if (!impl_->protocol || !impl_->error.empty()) return;
    try {
        if (impl_->trace) impl_->trace->record(1, 0x2E6, plaintext);
        impl_->protocol->warden(plaintext);
        if (!impl_->protocol->error().empty()) impl_->error = impl_->protocol->error();
    } catch (const std::exception& e) { impl_->error = e.what(); }
}
}
