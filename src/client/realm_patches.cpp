#include "client/realm_patches.hpp"

#include <openssl/sha.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <string_view>

namespace wowee::core {
namespace {

char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) c = lowerAscii(c);
    return s;
}

/// The names Windows resolves as devices wherever they appear, with or
/// without an extension: CON.mpq opens the console, not a file.
bool isWindowsDeviceName(std::string_view stem) {
    static constexpr std::string_view kDevices[] = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    for (std::string_view d : kDevices) {
        if (stem == d) return true;
    }
    return false;
}

}  // namespace

bool isSafePatchFileName(const std::string& name) {
    // Long enough to be "x.mpq" and short enough that no filesystem argues.
    if (name.size() < 5 || name.size() > 64) return false;

    // One pass over the bytes, because every rule below is about what the
    // name is made of. Anything outside this set is refused rather than
    // stripped - a separator, a colon, a null, a space, a control character
    // and every byte above ASCII all land here.
    for (char c : name) {
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && !digit && c != '.' && c != '-' && c != '_') return false;
    }

    // A dot run is how ".." hides in the middle of an otherwise ordinary
    // name, and a leading dot or dash is a hidden file or an argument.
    if (name.front() == '.' || name.front() == '-') return false;
    if (name.back() == '.') return false;
    if (name.find("..") != std::string::npos) return false;

    const std::string lower = toLowerAscii(name);
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".mpq") != 0) return false;

    // Checked on the stem, since the device is resolved whatever follows it.
    const std::string stem = lower.substr(0, lower.size() - 4);
    if (stem.empty()) return false;
    if (isWindowsDeviceName(stem)) return false;

    return true;
}

std::string realmPatchDirName(const std::string& host, int port) {
    // The hash first, over the address exactly as given, so that two hosts
    // differing only in what sanitising throws away still land in different
    // directories. Truncated because this is a directory name a person may
    // have to look at, and sixteen hex characters is far past collision by
    // accident - and nothing here defends against a deliberate one, because
    // the address comes from the player rather than from a server.
    std::string key = toLowerAscii(host);
    key += ':';
    key += std::to_string(port);

    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char*>(key.data()), key.size(), digest.data());

    std::string suffix;
    suffix.reserve(16);
    for (int i = 0; i < 8; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", digest[static_cast<std::size_t>(i)]);
        suffix += buf;
    }

    // The readable part: what a person would recognise, with everything that
    // is not plainly a hostname character folded to an underscore. It can
    // come out empty or absurd and it does not matter - the suffix is what
    // makes the name a name.
    std::string readable;
    readable.reserve(32);
    for (char c : toLowerAscii(host)) {
        const bool alpha = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const char mapped = (alpha || digit || c == '-' || c == '.') ? c : '_';
        // A dot run never survives, whatever it was before folding. Replacing
        // separators alone is not enough: "host/../.." folds to "host_.._.."
        // and carries "..", which is the one sequence this name must not
        // contain however harmlessly it got there.
        if (mapped == '.' && !readable.empty() && readable.back() == '.') continue;
        readable += mapped;
        if (readable.size() >= 32) break;
    }
    // A leading dot would make the whole directory hidden, and a trailing one
    // is refused by Windows.
    while (!readable.empty() && (readable.front() == '.' || readable.front() == '-')) {
        readable.erase(readable.begin());
    }
    while (!readable.empty() && readable.back() == '.') readable.pop_back();
    if (readable.empty()) readable = "realm";

    return readable + "-" + std::to_string(port) + "-" + suffix;
}

}  // namespace wowee::core
