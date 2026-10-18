#include "core/client_shortcuts.hpp"

namespace wowee::core {

namespace {

struct ShortcutKey {
    SDL_Scancode scancode;
    /// The modifiers held with it: SDL_KMOD_CTRL is either control key.
    SDL_Keymod modifiers;
    ClientShortcut shortcut;
    const char* label;
};

/// Every key the client keeps for itself. One place, so a new one is checked
/// against the rest and against what the interface is left with.
constexpr ShortcutKey kShortcuts[] = {
    {SDL_SCANCODE_GRAVE, SDL_KMOD_CTRL, ClientShortcut::TogglePerformanceHud, "Ctrl+`"},
    {SDL_SCANCODE_F10, SDL_KMOD_CTRL, ClientShortcut::ToggleWeatherPanel, "Ctrl+F10"},
};

/// Whether the modifiers held are the ones a shortcut names: each of control,
/// shift, alt and the system key held exactly when it is named. The locks
/// (caps, num, scroll) and AltGr's mode key are not modifiers of a shortcut.
bool sameModifiers(SDL_Keymod held, SDL_Keymod named) {
    constexpr SDL_Keymod kGroups[] = {SDL_KMOD_CTRL, SDL_KMOD_SHIFT, SDL_KMOD_ALT, SDL_KMOD_GUI};
    for (const SDL_Keymod group : kGroups) {
        if (((held & group) != 0) != ((named & group) != 0)) return false;
    }
    return true;
}

} // namespace

ClientShortcut clientShortcutFor(const SDL_KeyboardEvent& key) {
    for (const auto& entry : kShortcuts) {
        if (key.scancode == entry.scancode && sameModifiers(key.mod, entry.modifiers)) {
            return entry.shortcut;
        }
    }
    return ClientShortcut::None;
}

const char* clientShortcutLabel(ClientShortcut shortcut) {
    for (const auto& entry : kShortcuts) {
        if (entry.shortcut == shortcut) return entry.label;
    }
    return "";
}

} // namespace wowee::core
