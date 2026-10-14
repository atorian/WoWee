#pragma once

// The CVars: the named settings the interface reads and writes (GetCVar,
// SetCVar) and the game consults for its own behaviour - autoClearAFK,
// assistAttack, autoDismountFlying and the rest. One store, owned by the
// client and handed to whoever reads it, so the game, the interface and the
// addons all see the same value.

#include <string>
#include <string_view>
#include <unordered_map>

namespace wowee::core {

class CVarStore {
public:
    /// A store kept in `path`, or in memory only when it is empty - a test's.
    explicit CVarStore(std::string path = {});

    /// Read the file. Once per run: a panel reads its checkbox out of a CVar
    /// as it is built, so this happens before the interface loads, and after
    /// that the store is the file - every write goes to both. Later calls do
    /// nothing. A store that is not loaded answers every fallback.
    void load();

    /// The value, or `fallback` when nothing set it. Names are case-folded,
    /// as WoW's are: "uiScale" and "uiscale" are one CVar.
    [[nodiscard]] std::string get(std::string_view name, std::string_view fallback) const;
    /// The value, or nullptr when nothing set it.
    [[nodiscard]] const std::string* find(std::string_view name) const;

    /// Set it, and write the file when the value moved. Answers whether it
    /// did. Written on every change rather than at shutdown: the file is a few
    /// hundred bytes, and a setting that survives only a clean exit is not one
    /// a player can rely on - this client is exited every way there is.
    bool set(std::string_view name, std::string value);

    /// Every value, by folded name.
    [[nodiscard]] const std::unordered_map<std::string, std::string>& values() const { return values_; }

    /// The folded form of a name, as the store keys it.
    static std::string fold(std::string_view name);

private:
    void save() const;

    std::string path_;
    std::unordered_map<std::string, std::string> values_;
    bool loaded_ = false;
};

} // namespace wowee::core
