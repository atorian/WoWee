#include "collision/movement_plot.hpp"
#include <iostream>
#include <iomanip>
#include <string>
int main() {
    using namespace wowee::collision;
    std::cout<<std::setprecision(9)<<std::scientific;
    std::string name;
    while(std::cin>>name) {
        SurfaceMovementState s;uint32_t elapsed;glm::vec3 move;float facing,pitch;bool writeFacing,writePitch;
        if(!(std::cin>>elapsed>>s.flags>>s.flags2>>s.currentSpeed>>s.turnRate>>s.pitchRate
            >>s.anchorFacing>>s.anchorPitch>>s.direction.x>>s.direction.y>>s.direction.z
            >>s.direction2D.x>>s.direction2D.y>>s.pitchCos>>s.pitchSin
            >>move.x>>move.y>>move.z>>facing>>pitch>>writeFacing>>writePitch))return 2;
        auto active=plotMovement(s,elapsed,move,writeFacing?&facing:nullptr,writePitch?&pitch:nullptr);
        std::cout<<"{\"name\":\""<<name<<"\",\"active\":"<<active<<",\"move\":["<<move.x<<','<<move.y<<','<<move.z
            <<"],\"facing\":"<<facing<<",\"pitch\":"<<pitch<<"}\n";
    }
    return std::cin.eof()?0:2;
}
