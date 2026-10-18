#pragma once

namespace wowee {
namespace game { class GameHandler; }
namespace rendering { class Renderer; }

namespace ui {

/**
 * The weather dev panel: put any weather on screen and watch it live.
 *
 * Shows the weather the realm says and the weather being drawn, part by part,
 * and sets the renderer's WeatherControls - a weather and a wind of its own
 * over the realm's and the zone's, how fast a change comes over, and which
 * parts are drawn (what falls, the splashes, the lens, the lightning). What it
 * sets stays until it is set back: the realm's weather comes back the moment
 * the override is unticked.
 *
 * Toggled with the client's own shortcut (core::ClientShortcut).
 */
class WeatherPanel {
public:
    void toggle() { open_ = !open_; }
    [[nodiscard]] bool isOpen() const { return open_; }

    /// Inside the ImGui frame. `game` may be null before a session.
    void draw(rendering::Renderer& renderer, const game::GameHandler* game);

private:
    bool open_ = false;
};

}  // namespace ui
}  // namespace wowee
