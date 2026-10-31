#pragma once

namespace wowee::game {

class GameData;
class GameHandler;

/// Give the game the wire half of an expansion profile: its opcode table, its
/// update-field table and the packet parsers built for it, each made active as
/// it loads.
///
/// The client calls this at startup and on an expansion change, and a test
/// calls it to talk WotLK to a GameHandler with no client - one copy, so a
/// table added here reaches both. The files are the running expansion's, from
/// its list; one that is not there or does not load stops the game.
void loadProtocolTables(GameHandler& game, const GameData& data);

} // namespace wowee::game
