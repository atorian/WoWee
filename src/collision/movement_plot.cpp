#include "collision/movement_plot.hpp"
#include <cmath>

namespace wowee::collision {
namespace {
constexpr uint32_t translating=0xc0100f;
constexpr float diagonal=.70710677f,twoPi=6.2831855f;
double rate(const SurfaceMovementState& s,bool pitch) {
    const uint32_t positive=pitch?0x40:0x10,negative=pitch?0x80:0x20;
    double r=(s.flags&positive)?(pitch?s.pitchRate:s.turnRate):
        (s.flags&negative)?-double(pitch?s.pitchRate:s.turnRate):0;
    if((s.flags&translating)&&!(s.flags2&(pitch?0x10:8)))r*=.75f;
    return r;
}
struct Arc {double forward,side;};
Arc arc(double speed,double r,float dt) {
    // Original FSINCOS arguments/results and radius are stored as binary32;
    // subsequent products and differences retain x87 register precision.
    const float radius=float(speed/r),angle=float(r*dt);
    const float cosine=float(std::cos(double(angle))),sine=float(std::sin(double(angle)));
    return {double(sine)*radius,double(radius)-double(cosine)*radius};
}
}
// 0x987b50, including the original mode table and early return at zero time.
uint32_t plotMovement(const SurfaceMovementState& s,uint32_t time,glm::vec3& move,float* facing,float* pitch) {
    if(!time)return s.flags&translating;
    double dt=double(time)*.001f;
    uint32_t mode=0;
    if(s.flags&0x30) {
        if(facing) {
            dt=float(dt);
            double angle=std::fmod(rate(s,false)*dt+s.anchorFacing,double(twoPi));
            if(angle<0)angle+=twoPi;
            *facing=float(angle);
        }
        if(!(s.flags&0x1000))mode=2;
    }
    if(s.flags&0xc0) {
        if(pitch) {
            dt=float(dt);
            *pitch=float(std::fmod(rate(s,true)*dt+s.anchorPitch,double(twoPi)));
        }
        if(!(s.flags&0xc00000))mode|=8;
    }
    if(s.flags&0xc00000)mode|=16;
    if(s.flags&3)mode|=1;
    if(s.flags&12)mode|=4;
    const double duration=float(dt),speed=s.currentSpeed;
    switch(mode) {
        case 1:case 4:case 5:case 12:
            move={double(s.direction.x)*duration*speed,double(s.direction.y)*duration*speed,duration*s.direction.z*speed};
            break;
        case 3:case 6:case 7:case 14:case 19:case 22:case 23: {
            const auto a=arc(speed,rate(s,false),float(duration));
            double x=double(s.direction2D.x)*a.forward-double(s.direction2D.y)*a.side;
            double y=a.forward*s.direction2D.y+a.side*s.direction2D.x;
            double z=0;
            if(s.flags&0xc00000) {
                x*=diagonal;y*=diagonal;
                z=(speed*duration)*((s.flags&0x400000)?diagonal:-diagonal);
            } else if(s.flags&3) {
                x*=s.pitchCos;y*=s.pitchCos;z=double(s.pitchSin)*speed*duration;
            }
            move={x,y,z};break;
        }
        case 9:case 13: {
            const auto a=arc(speed,rate(s,true),float(duration));
            const double horizontal=a.forward*s.pitchCos-a.side*s.pitchSin;
            move={horizontal*s.direction2D.x,horizontal*s.direction2D.y,a.side*s.pitchCos+a.forward*s.pitchSin};break;
        }
        case 11:case 15: {
            const auto horizontal=arc(speed*diagonal,rate(s,false),float(duration));
            const auto vertical=arc(speed*diagonal,rate(s,true),float(duration));
            move={double(s.direction2D.x)*horizontal.forward-double(s.direction2D.y)*horizontal.side,
                horizontal.forward*s.direction2D.y+horizontal.side*s.direction2D.x,
                vertical.side*s.pitchCos+vertical.forward*s.pitchSin};break;
        }
        case 16:case 18:
            move={0,0,(s.flags&0x400000)?dt*speed:-dt*speed};break;
        case 17:case 20:case 21:
            move={((double(s.direction2D.x)*diagonal)*duration)*speed,
                ((double(s.direction2D.y)*diagonal)*duration)*speed,
                speed*(((s.flags&0x400000)?double(diagonal):-double(diagonal))*duration)};break;
        default:break;
    }
    return s.flags&translating;
}
} // namespace wowee::collision
