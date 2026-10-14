#pragma once

namespace wowee::game {

class GameHandler;
struct ExpansionProfile;

/// Give the game the wire half of an expansion profile: its opcode table, its
/// update-field table and the packet parsers built for it, each made active as
/// it loads.
///
/// The client calls this at startup and on an expansion change, and a test
/// calls it to talk WotLK to a GameHandler with no client - one copy, so a
/// table added here reaches both. A table that fails to load is logged and the
/// previous one stays active: a wrong table reads better than a null one.
void loadProtocolTables(GameHandler& game, const ExpansionProfile& profile);

} // namespace wowee::game
