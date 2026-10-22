#include "collision/movement_commands.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <stdexcept>

using namespace wowee::collision;
struct Sink final:MovementCommandSink {
    SurfaceMovementState& state;
    SurfaceMovementShape& shape;
    bool first=true;
    Sink(SurfaceMovementState& s,SurfaceMovementShape& h):state(s),shape(h){}
    void movementEffect(const MovementCommandEffect&) override {throw std::logic_error("Mode effect outside height fixture");}
    bool forcedMovementChanged(uint32_t,uint32_t,float,uint32_t,bool) override {throw std::logic_error("Rate outside height-only fixture");}
    bool inputChanged(uint32_t,uint32_t,bool) override {throw std::logic_error("Input outside height-only fixture");}
    void angularInputStopped(bool) override {throw std::logic_error("Angular input outside height-only fixture");}
    void resetHeartbeat(uint32_t) override {throw std::logic_error("Heartbeat reset outside height-only fixture");}
    void collisionHeightChanged(uint32_t time,float height,uint32_t counter,bool ack) override {
        if(!first)std::cout<<',';
        first=false;
        std::cout<<'['<<time<<",1303,"<<ack<<','<<height<<','<<counter<<",0,255,"
            <<shape.height<<','<<state.moveStartDelayMs<<','<<state.flags2<<']';
    }
};
int main() {
    std::cout<<std::scientific<<std::setprecision(9);
    std::string name;
    while(std::cin>>name) {
        SurfaceMovementState s;SurfaceMovementShape shape{.33333334f,2.0277777f,1,true};
        uint32_t count;
        if(!(std::cin>>s.flags>>s.flags2>>s.moveStartDelayMs>>shape.height>>count))return 2;
        MovementCommandQueue queue;Sink sink(s,shape);bool first=true;
        std::cout<<"{\"name\":\""<<name<<"\",\"updates\":[";
        for(uint32_t i=0;i<count;++i) {
            uint32_t kind,time,counter;bool flag;float height;
            if(!(std::cin>>kind>>time))return 2;
            if(!kind) {
                if(!(std::cin>>height>>counter>>flag))return 2;
                queue.enqueueCollisionHeight(time,height,counter,flag);
            } else {
                if(!(std::cin>>flag))return 2;
                if(!first)std::cout<<',';
                first=false;sink.first=true;
                std::cout<<"{\"notifications\":[";
                const auto result=queue.process(time,flag,false,s,shape,sink);
                std::cout<<"],\"result\":"<<int(result)<<",\"height\":"<<shape.height
                    <<",\"delay\":"<<s.moveStartDelayMs<<",\"flags2\":"<<s.flags2
                    <<",\"pending\":"<<queue.size()<<",\"next\":";
                if(auto next=queue.nextTime())std::cout<<*next;else std::cout<<"null";
                std::cout<<'}';
            }
        }
        std::cout<<"]}\n";
    }
    return std::cin.eof()?0:2;
}
