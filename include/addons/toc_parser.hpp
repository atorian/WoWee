#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

struct TocFile {
    std::string addonName;
    std::string basePath;

    std::unordered_map<std::string, std::string> directives;
    std::vector<std::string> files;

    [[nodiscard]] std::string getTitle() const;
    [[nodiscard]] bool isLoadOnDemand() const;
    [[nodiscard]] std::vector<std::string> getSavedVariables() const;
    [[nodiscard]] std::vector<std::string> getSavedVariablesPerCharacter() const;

    /// Where this addon's account-wide saved variables are kept.
    [[nodiscard]] std::string savedVariablesPath() const;
    /// Where one character's saved variables of this addon are kept, keyed by
    /// the character's name; empty when there is no name to key them by, so
    /// nothing is written to a file that belongs to nobody.
    [[nodiscard]] std::string savedVariablesPerCharacterPath(const std::string& characterName) const;
};

std::optional<TocFile> parseTocFile(const std::string& tocPath);

} // namespace wowee::addons
