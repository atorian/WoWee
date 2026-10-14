#include "core/cvar_store.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace wowee::core {

CVarStore::CVarStore(std::string path) : path_(std::move(path)) {}

std::string CVarStore::fold(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

void CVarStore::load() {
    if (loaded_) return;
    loaded_ = true;
    if (path_.empty()) return;
    std::ifstream in(path_);
    if (!in.is_open()) {
        // A first run has no file yet, and that is not a fault: every CVar
        // answers its default until something is set.
        LOG_INFO("CVars: no ", path_, " yet; every CVar is at its default");
        return;
    }
    std::string line;
    size_t loaded = 0;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        values_[fold(std::string_view(line).substr(0, eq))] = line.substr(eq + 1);
        ++loaded;
    }
    LOG_INFO("CVars: loaded ", loaded, " from ", path_);
}

std::string CVarStore::get(std::string_view name, std::string_view fallback) const {
    if (const std::string* value = find(name)) return *value;
    return std::string(fallback);
}

const std::string* CVarStore::find(std::string_view name) const {
    auto it = values_.find(fold(name));
    return it != values_.end() ? &it->second : nullptr;
}

bool CVarStore::set(std::string_view name, std::string value) {
    std::string key = fold(name);
    auto it = values_.find(key);
    if (it != values_.end() && it->second == value) return false;
    values_[std::move(key)] = std::move(value);
    save();
    return true;
}

void CVarStore::save() const {
    if (path_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream out(path_);
    if (!out.is_open()) {
        LOG_WARNING("CVars: could not write ", path_, "; this change lasts until the client closes");
        return;
    }
    // Sorted, so a diff of the file reads as a change of settings rather than
    // as the hash order moving underneath it.
    std::vector<const std::pair<const std::string, std::string>*> rows;
    rows.reserve(values_.size());
    for (const auto& kv : values_) rows.push_back(&kv);
    std::sort(rows.begin(), rows.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    for (const auto* kv : rows) {
        // A value with a newline in it would come back as two broken lines.
        if (kv->second.find('\n') != std::string::npos) {
            LOG_WARNING("CVars: '", kv->first, "' holds a line break and is not saved");
            continue;
        }
        out << kv->first << '=' << kv->second << '\n';
    }
}

} // namespace wowee::core
