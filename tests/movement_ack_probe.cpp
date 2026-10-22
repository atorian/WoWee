#include "core/movement_status.hpp"
#include "collision/movement_surface.hpp"
#include "core/logger.hpp"
#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);
    wowee::game::OpcodeTable table;
    if (!table.loadFromJson(argv[1])) return 2;
    wowee::game::setActiveOpcodeTable(&table);
    std::string name;
    while (std::cin >> name) {
        wowee::collision::SurfaceMovementState s;
        uint32_t opcode, counter, time;
        uint64_t guid;
        float value;
        if (!(std::cin >> opcode >> guid >> counter >> value >> s.flags >> s.flags2 >> time
            >> s.position.x >> s.position.y >> s.position.z >> s.facing >> s.pitch >> s.fallTimeMs
            >> s.fallVelocity >> s.direction2D.x >> s.direction2D.y >> s.currentSpeed >> s.stepStartZ)) return 2;
        const auto logical = table.fromWire(static_cast<uint16_t>(opcode));
        if (!logical) return 2;
        const auto status = wowee::core::makeUnattachedMovementStatus12340(s, time);
        const auto packet = wowee::game::MovementPacket::buildForceAck(*logical, status, guid, counter, value);
        std::ostringstream bytes;
        for (auto b : packet.getData()) bytes << std::hex << std::setfill('0') << std::setw(2) << unsigned(b);
        std::cout << "{\"name\":\"" << name << "\",\"opcode\":" << packet.getOpcode()
                  << ",\"body\":\"" << bytes.str() << "\"}\n";
    }
    return std::cin.eof() ? 0 : 2;
}
