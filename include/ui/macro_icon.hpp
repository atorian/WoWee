#pragma once

#include <string>

namespace wowee::game { struct Macro; class GameHandler; }
namespace wowee::rendering { class Renderer; }

namespace wowee::ui {
// Presentation only: never executes commands or changes the saved icon/body.
// A question-mark icon selects the spell/item named by the macro.
std::string macroIconPath(const game::Macro& macro, game::GameHandler& game,
                          rendering::Renderer* renderer = nullptr);
}
