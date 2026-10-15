#include "game/account_data.hpp"

#include <charconv>
#include <sstream>

namespace wowee {
namespace game {

const char* accountDataName(AccountDataType type) {
    switch (type) {
        case AccountDataType::GlobalConfig:      return "account config-cache.wtf";
        case AccountDataType::CharacterConfig:   return "character config-cache.wtf";
        case AccountDataType::GlobalBindings:    return "account bindings-cache.wtf";
        case AccountDataType::CharacterBindings: return "character bindings-cache.wtf";
        case AccountDataType::GlobalMacros:      return "account macros-cache.txt";
        case AccountDataType::CharacterMacros:   return "character macros-cache.txt";
        case AccountDataType::CharacterLayout:   return "character layout-cache.txt";
        case AccountDataType::CharacterChat:     return "character chat-cache.txt";
    }
    return "unknown account data";
}

namespace {

/// The lines of a text, without their CR or LF.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

/// "MACRO <id> "<name>" <icon>", or false when the header is not one.
bool parseMacroHeader(const std::string& line, Macro& out) {
    static constexpr std::string_view kTag = "MACRO ";
    if (line.compare(0, kTag.size(), kTag) != 0) return false;
    size_t pos = kTag.size();
    const size_t idEnd = line.find(' ', pos);
    if (idEnd == std::string::npos) return false;
    uint32_t id = 0;
    const auto [ptr, ec] = std::from_chars(line.data() + pos, line.data() + idEnd, id);
    if (ec != std::errc() || ptr != line.data() + idEnd || id == 0) return false;
    // The name is quoted and may hold spaces; the icon follows the closing quote.
    const size_t open = line.find('"', idEnd);
    if (open == std::string::npos) return false;
    const size_t close = line.find('"', open + 1);
    if (close == std::string::npos) return false;
    out.id = id;
    out.name = line.substr(open + 1, close - open - 1);
    std::string icon = line.substr(close + 1);
    const size_t first = icon.find_first_not_of(' ');
    out.icon = first == std::string::npos ? std::string() : icon.substr(first);
    return true;
}

} // namespace

std::vector<Macro> parseMacrosCache(const std::string& text, std::vector<std::string>* problems) {
    std::vector<Macro> macros;
    const auto lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;
        Macro macro;
        if (!parseMacroHeader(line, macro)) {
            if (problems) problems->push_back("line " + std::to_string(i + 1) +
                                              " is not a MACRO header: \"" + line + "\"");
            continue;
        }
        bool ended = false;
        std::string body;
        for (++i; i < lines.size(); ++i) {
            if (lines[i] == "END") { ended = true; break; }
            if (!body.empty()) body += '\n';
            body += lines[i];
        }
        if (!ended) {
            if (problems) problems->push_back("macro " + std::to_string(macro.id) + " \"" +
                                              macro.name + "\" has no END");
            break;
        }
        macro.body = std::move(body);
        macros.push_back(std::move(macro));
    }
    return macros;
}

std::string writeMacrosCache(const std::vector<Macro>& macros) {
    std::string out;
    for (const Macro& macro : macros) {
        out += "MACRO " + std::to_string(macro.id) + " \"" + macro.name + "\" " + macro.icon + "\r\n";
        std::istringstream body(macro.body);
        std::string line;
        while (std::getline(body, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out += line + "\r\n";
        }
        out += "END\r\n";
    }
    return out;
}

std::vector<KeyBindingLine> parseBindingsCache(const std::string& text,
                                               std::vector<std::string>* problems) {
    std::vector<KeyBindingLine> out;
    const auto lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty() || line.rfind("BINDINGMODE", 0) == 0) continue;
        static constexpr std::string_view kTag = "bind ";
        // The key never holds a space ("SHIFT-MOUSEWHEELUP", "BUTTON3"), and
        // the command may: "SPELL Frost Nova", "CLICK MyButton:LeftButton".
        const size_t keyEnd = line.find(' ', kTag.size());
        if (line.compare(0, kTag.size(), kTag) != 0 || keyEnd == std::string::npos ||
            keyEnd == kTag.size() || keyEnd + 1 >= line.size()) {
            if (problems) problems->push_back("line " + std::to_string(i + 1) +
                                              " is not a binding: \"" + line + "\"");
            continue;
        }
        out.push_back({line.substr(kTag.size(), keyEnd - kTag.size()), line.substr(keyEnd + 1)});
    }
    return out;
}

std::string writeBindingsCache(const std::vector<KeyBindingLine>& lines) {
    std::string out = "BINDINGMODE 0\r\n";
    for (const auto& line : lines) out += "bind " + line.key + " " + line.command + "\r\n";
    return out;
}

} // namespace game
} // namespace wowee
