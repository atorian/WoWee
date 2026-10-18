#pragma once

#include <SDL3/SDL.h>

namespace wowee {
namespace core {

/// What the client does itself with a key press, ahead of the interface.
enum class ClientShortcut {
    None,
    /// Show or hide the performance HUD.
    TogglePerformanceHud,
    /// Show or hide the weather dev panel.
    ToggleWeatherPanel,
};

/// The client's own shortcut a key press is, or None.
///
/// These presses belong to the client and never to the interface: one that
/// matches is not offered to the interface's key bindings, its edit boxes or
/// chat. Everything else is the player's to bind - the HUD sat on F1, which
/// every character's bindings already use, so the key did one thing or the
/// other depending on which got to it first.
///
/// Matched by scancode, the key's place on the keyboard, so it is the same key
/// on every layout; and with exactly the modifiers it names - left or right
/// of each - and no others, so Ctrl+Shift+` is not Ctrl+` and a plain ` is
/// the interface's like any other key.
[[nodiscard]] ClientShortcut clientShortcutFor(const SDL_KeyboardEvent& key);

/// How the shortcut is written for the player, e.g. "Ctrl+`".
[[nodiscard]] const char* clientShortcutLabel(ClientShortcut shortcut);

} // namespace core
} // namespace wowee
