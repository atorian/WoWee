#include "core/movement_status.hpp"
#include "collision/movement_surface.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

std::string hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    for(auto b:bytes)out<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(b);
    return out.str();
}
int main() {
    std::string name,original;
    while(std::cin>>name) {
        wowee::collision::SurfaceMovementState s;uint32_t time;
        if(!(std::cin>>s.flags>>s.flags2>>time>>s.position.x>>s.position.y>>s.position.z
            >>s.facing>>s.pitch>>s.fallTimeMs>>s.fallVelocity>>s.direction2D.x>>s.direction2D.y
            >>s.currentSpeed>>s.stepStartZ>>original))return 2;
        const auto status=wowee::core::makeUnattachedMovementStatus12340(s,time);
        wowee::network::Packet written;
        wowee::game::MovementPacket::writeMovementPayload(written,status);
        std::vector<uint8_t> bytes;
        for(size_t i=0;i<original.size();i+=2)bytes.push_back(uint8_t(std::stoul(original.substr(i,2),nullptr,16)));
        wowee::network::Packet received(0,std::move(bytes)),rewritten;
        wowee::game::MovementInfo decoded;
        const bool read=wowee::game::MovementPacket::readMovementPayload(received,decoded);
        if(read)wowee::game::MovementPacket::writeMovementPayload(rewritten,decoded);
        std::cout<<"{\"name\":\""<<name<<"\",\"written\":\""<<hex(written.getData())
            <<"\",\"read_rewritten\":\""<<hex(rewritten.getData())<<"\",\"read\":"<<(read?"true":"false")
            <<",\"remaining\":"<<received.getRemainingSize()<<"}\n";
    }
    return std::cin.eof()?0:2;
}
