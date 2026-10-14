#pragma once

// Small string helpers every layer can use. Header-only: wowee_base has no
// source for these, and the auth library, the pipeline and the interface all
// include them.

#include <string>
#include <string_view>

namespace wowee::core {

/// `text` without the spaces, tabs, carriage returns and line feeds at either
/// end. ASCII only and locale-free, unlike std::isspace: a host name, a PIN
/// and a chat line are trimmed the same way on every machine.
///
/// The auth handler, the login screen and the chat helpers each wrote this
/// out, with the same four characters.
inline std::string trimAsciiSpace(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) return {};
    const size_t last = text.find_last_not_of(kSpace);
    return std::string(text.substr(first, last - first + 1));
}

} // namespace wowee::core
