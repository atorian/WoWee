#include "game/protocol_tables.hpp"

#include "game/expansion_profile.hpp"
#include "game/game_data.hpp"
#include "game/game_handler.hpp"
#include "game/opcode_table.hpp"
#include "game/packet_parsers.hpp"
#include "game/update_field_table.hpp"
#include "core/logger.hpp"

#include <memory>
#include <string>

namespace wowee::game {

void loadProtocolTables(GameHandler& game, const GameData& data) {
    const ExpansionProfile& profile = data.expansion();
    const std::string opcodesPath = data.file("opcodes.json", "the world server's opcodes");
    if (!game.getOpcodeTable().loadFromJson(opcodesPath)) {
        data.stop(opcodesPath, "the world server's opcodes", "holds no opcode the client knows");
    }
    setActiveOpcodeTable(&game.getOpcodeTable());

    const std::string updateFieldsPath = data.file("update_fields.json", "the objects' update fields");
    if (!game.getUpdateFieldTable().loadFromJson(updateFieldsPath)) {
        data.stop(updateFieldsPath, "the objects' update fields", "could not be read");
    }
    setActiveUpdateFieldTable(&game.getUpdateFieldTable());

    // An id with no parser set is a profile this build cannot read the wire
    // for. It still runs, on WotLK's parsers, because refusing to start would
    // break a realm running a hand-written profile that happens to be close
    // enough - but it says so, which is the half that was missing.
    auto packetParsers = createPacketParsers(profile.id);
    if (!packetParsers) {
        LOG_ERROR("No packet parsers for expansion '", profile.id,
                  "'. Falling back to WotLK, which will misparse movement and "
                  "update-object against any realm that is not 3.3.5a");
        packetParsers = std::make_unique<WotlkPacketParsers>();
    }
    game.setPacketParsers(std::move(packetParsers));
}

} // namespace wowee::game
