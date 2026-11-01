#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::title {

/// One CharTitles.dbc row.
struct TitleRecord {
    /// The row id a quest reward references.
    uint32_t id = 0;
    /// The bit off the player's own fields, for a title that can be worn.
    uint32_t bit = 0;
    /// A sentence with a hole in it: "%s the Explorer", "Sergeant %s".
    std::string format;
};

/// A title by its bit or its id, with a name put where the row says.
class TitleCatalog {
public:
    void describe(const std::vector<TitleRecord>& rows);
    /// Empty when the bit is unknown.
    [[nodiscard]] std::string byBit(uint32_t bit, const std::string& name) const;
    /// Empty when the id is unknown.
    [[nodiscard]] std::string byId(uint32_t id, const std::string& name) const;

private:
    std::unordered_map<uint32_t, std::string> byBit_;
    std::unordered_map<uint32_t, std::string> byId_;
};

/// The name goes where the row says and not before it.
std::string formatTitle(const std::string& format, const std::string& name);

} // namespace wowee::title
