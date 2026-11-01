#include "core/movement_reporting.hpp"
#include "collision/movement_execution.hpp"
#include "core/logger.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

struct Connection: wowee::core::MovementReportConnection {
    wowee::core::MovementReportingState& reporting;
    wowee::collision::MovementClock& clock;
    std::string packets,atSend;
    unsigned count=0;
    Connection(wowee::core::MovementReportingState& r,wowee::collision::MovementClock& c):reporting(r),clock(c){}
    void sendMovement(const wowee::network::Packet& packet) override {
        std::ostringstream bytes;
        for(auto b:packet.getData())bytes<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(b);
        if(count++){packets+=",";atSend+=",";}
        packets+="{\"opcode\":"+std::to_string(packet.getOpcode())+",\"body\":\""+bytes.str()+"\"}";
        atSend+="["+std::to_string(reporting.unitFlags)+","+std::to_string(clock.heartbeatTimeMs)+"]";
    }
};
struct Effects: wowee::core::MovementLocalEffects {
    Connection& connection;
    std::string events;
    explicit Effects(Connection& c):connection(c){}
    void record(const std::string& event) {
        if(!events.empty())events+=",";
        events+="["+event+","+std::to_string(connection.count)+","+
            std::to_string(connection.reporting.unitFlags)+","+
            std::to_string(connection.clock.heartbeatTimeMs)+"]";
    }
    void refreshMovementEffects() override { record("1"); }
    void standUp() override { record("2,0"); }
    void updateInput(uint32_t time) override { record("3,"+std::to_string(time)+",1"); }
    void movementBecameIdle() override { record("4"); }
    void notifyMovement(wowee::game::Opcode opcode) override { record("5,"+std::to_string(wowee::game::wireOpcode(opcode))); }
};
int main(int argc,char** argv) {
    if(argc!=2&&argc!=3)return 2;
    const bool local=argc==3&&std::string(argv[2])=="--local";
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);
    wowee::game::OpcodeTable table;
    if(!table.loadFromJson(argv[1]))return 2;
    wowee::game::setActiveOpcodeTable(&table);
    std::string name;
    while(std::cin>>name) {
        wowee::collision::SurfaceMovementState s;
        wowee::core::MovementReportingState report;
        wowee::collision::MovementClock clock;
        uint32_t opcode,counter,time;
        uint64_t guid;
        float value;
        bool active;
        if(!(std::cin>>opcode>>guid>>counter>>value>>s.flags>>s.flags2>>time
            >>s.position.x>>s.position.y>>s.position.z>>s.facing>>s.pitch>>s.fallTimeMs
            >>s.fallVelocity>>s.direction2D.x>>s.direction2D.y>>s.currentSpeed>>s.stepStartZ
            >>active>>report.unitFlags>>clock.heartbeatTimeMs>>report.facing>>report.pitch))return 2;
        const auto logical=table.fromWire(static_cast<uint16_t>(opcode));
        if(!logical)return 2;
        Connection connection(report,clock);
        Effects effects(connection);
        bool accepted;
        if(local) {
            bool needAck,player,flush;
            uint32_t stand,pendingTurn,pendingPitch;
            if(!(std::cin>>needAck>>stand>>player>>report.turnTimeMs>>report.pitchTimeMs>>pendingTurn>>pendingPitch>>flush))return 2;
            if(pendingTurn)report.pendingTurn=table.fromWire(static_cast<uint16_t>(pendingTurn));
            if(pendingPitch)report.pendingPitch=table.fromWire(static_cast<uint16_t>(pendingPitch));
            accepted=flush?wowee::core::flushTimedMovementReports12340(s,report,clock,connection,guid,active,time):
                wowee::core::processLocalMovementEvent12340(s,report,clock,connection,effects,guid,active,player,stand,time,*logical,needAck,counter,value);
        } else accepted=wowee::core::reportUnattachedMovement12340(s,report,clock,connection,guid,active,time,*logical,counter,value);
        std::cout<<std::setprecision(17)<<"{\"name\":\""<<name<<"\",\"accepted\":"<<(accepted?"true":"false")
            <<",\"packets\":["<<connection.packets<<"],\"at_send\":["<<connection.atSend
            <<"],\"unit_flags\":"<<report.unitFlags<<",\"heartbeat_time\":"<<clock.heartbeatTimeMs
            <<",\"reported_facing\":"<<report.facing<<",\"reported_pitch\":"<<report.pitch;
        if(local)std::cout<<",\"effects\":["<<effects.events<<"],\"turn_time\":"<<report.turnTimeMs
            <<",\"pitch_time\":"<<report.pitchTimeMs<<",\"pending_turn\":"<<(report.pendingTurn?wowee::game::wireOpcode(*report.pendingTurn):0)
            <<",\"pending_pitch\":"<<(report.pendingPitch?wowee::game::wireOpcode(*report.pendingPitch):0);
        std::cout<<"}\n";
    }
    return std::cin.eof()?0:2;
}
