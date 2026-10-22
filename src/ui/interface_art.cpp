#include "ui/interface_art.hpp"
#include "ui/xml_parser.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

namespace wowee::ui {
namespace {
std::string normalize(std::string text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '/') c = '\\';
        if (c == '\\' && !out.empty() && out.back() == '\\') continue;
        out += static_cast<char>(std::tolower(c));
    }
    return out;
}
bool characterArt(const std::string& path) { return path.starts_with("interface\\icons\\"); }
}
std::vector<InterfaceArt> interfaceArtReferences(const std::string& source,
                                                const std::vector<std::string>& texturePaths) {
    std::set<std::pair<std::string, bool>> found;
    // Lua and XML both quote literal asset names. A filename prefix may be
    // concatenated with a state/number (bag backgrounds, panel sections).
    // Expand that prefix against real assets, never an entire dynamic folder.
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '\'' && source[i] != '"') continue;
        // Ignore punctuation in comments/text; only a quote immediately
        // followed by an Interface path can start a resource literal.
        if (normalize(source.substr(i + 1, 10)) != "interface\\") continue;
        const char quote = source[i];
        const size_t end = source.find(quote, i + 1);
        if (end == std::string::npos) break;
        const auto path = normalize(source.substr(i + 1, end - i - 1));
        i = end;
        if (!path.starts_with("interface\\") || characterArt(path) || path.back() == '\\') continue;
        auto prefix = path;
        if (prefix.ends_with(".tga")) prefix.replace(prefix.size() - 4, 4, ".blp");
        const bool exact = prefix.ends_with(".blp");
        if (exact) found.emplace(prefix, false);
        else {
            for (auto it = std::lower_bound(texturePaths.begin(), texturePaths.end(), prefix);
                 it != texturePaths.end() && it->starts_with(prefix); ++it)
                found.emplace(*it, false);
        }
    }
    // Keep additive images distinct. XML declares them even on hidden
    // templates; normal and additive use the same decode/cache identity rules.
    XmlNode root;
    std::string error;
    if (source.find("<Ui") != std::string::npos && parseXml(source, root, error)) {
        const auto visit = [&](const auto& self, const XmlNode& node) -> void {
            const auto mode = node.attrOr("alphaMode", node.attrOr("blendMode", ""));
            if (mode == "ADD") if (const auto* file = node.attr("file")) {
                auto path = normalize(*file);
                if (path.starts_with("interface\\") && !characterArt(path)) {
                    if (path.ends_with(".tga")) path.resize(path.size() - 4);
                    if (!path.ends_with(".blp")) path += ".blp";
                    found.emplace(std::move(path), true);
                }
            }
            for (const auto& child : node.children) self(self, child);
        };
        visit(visit, root);
    }
    std::vector<InterfaceArt> result;
    for (const auto& [path, add] : found) result.push_back({path, add});
    return result;
}
std::vector<InterfaceArt> discoverInterfaceArt(const std::string& directory,
                                              const std::vector<std::string>& paths) {
    std::set<std::pair<std::string, bool>> found;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(directory, ec), end;
    if (ec) {
        LOG_WARNING("Interface prefetch: cannot scan '", directory, "': ", ec.message(),
                    "; art will be queued on demand");
        return {};
    }
    for (; it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const auto extension = normalize(it->path().extension().string());
        if (extension != ".xml" && extension != ".lua") continue;
        std::ifstream file(it->path(), std::ios::binary);
        if (!file) {
            LOG_WARNING("Interface prefetch: cannot read '", it->path().string(), "'");
            continue;
        }
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        for (const auto& ref : interfaceArtReferences(source, paths)) found.emplace(ref.path, ref.additive);
    }
    if (ec) LOG_WARNING("Interface prefetch scan failed: ", ec.message(), "; remaining art loads on demand");
    std::vector<InterfaceArt> result;
    for (const auto& [path, add] : found) result.push_back({path, add});
    return result;
}
}
