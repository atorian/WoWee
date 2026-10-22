#include "collision/movement_surface.hpp"
#include "collision/movement_query.hpp"
#include "collision/movement_request.hpp"
#include "collision/detail/movement_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>

namespace wowee::collision {
namespace {
using C3Vector = glm::vec3;
using C2Vector = glm::vec2;
using C4Plane = MovementPlane;
using CFacet = MovementFacet;
bool fnotequal(float a, float b) { return std::abs(double(a)-b) >= 9.5367432e-7f; }
bool fequal(float a, float b) { return !fnotequal(a,b); }
struct WOWGUID { uint64_t m_guid = 0; };
constexpr uint32_t MOVEMENTFLAG_FORWARD=1,MOVEMENTFLAG_BACKWARD=2,MOVEMENTFLAG_STRAFE_LEFT=4,MOVEMENTFLAG_STRAFE_RIGHT=8,
MOVEMENTFLAG_PITCH_UP=0x40,MOVEMENTFLAG_PITCH_DOWN=0x80,MOVEMENTFLAG_WALKING=0x100,MOVEMENTFLAG_ONTRANSPORT=0x200,MOVEMENTFLAG_ROOT=0x800,
MOVEMENTFLAG_FALLING=0x1000,MOVEMENTFLAG_FALLING_FAR=0x2000,MOVEMENTFLAG_PENDING_ROOT=0x100000,MOVEMENTFLAG_SWIMMING=0x200000,
MOVEMENTFLAG_ASCENDING=0x400000,MOVEMENTFLAG_DESCENDING=0x800000,MOVEMENTFLAG_FLYING=0x2000000,MOVEMENTFLAG_SPLINE_ELEVATION=0x4000000,
MOVEMENTFLAG_SAFE_FALL=0x20000000,MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING=0x20,
MOVEMENTFLAG_PENDING_STOP=0x4000,MOVEMENTFLAG_PENDING_STRAFE_STOP=0x8000,
MOVEMENTFLAG_PENDING_FORWARD=0x10000,MOVEMENTFLAG_PENDING_BACKWARD=0x20000,
MOVEMENTFLAG_PENDING_STRAFE_LEFT=0x40000,MOVEMENTFLAG_PENDING_STRAFE_RIGHT=0x80000,
MOVEMENTFLAG2_NO_STRAFE=1;

struct RequestGeometry {
    MovementFacetSource& source;
    bool local,ghost;
    MovementRequestCache& cache;
    MovementGeometryStatus status=MovementGeometryStatus::Ready;
    std::vector<MovementRequestEvent>* events=nullptr;
    bool acquire(const MovementFacetRequest& request) {
        cache.solids.clear();
        auto next=source.query(request,request.solidFlags,cache.solids);
        if (next!=MovementGeometryStatus::Ready) { status=next;return false; }
        cache.liquids.clear();
        if (request.queryLiquid) {
            next=source.query(request,0x20000,cache.liquids);
            if (next!=MovementGeometryStatus::Ready) status=next;
            for (auto& facet:cache.liquids) {
                facet.plane.normal=-facet.plane.normal;
                facet.plane.offset=-facet.plane.offset;
            }
        }
        cache.request=request;
        return true;
    }
};

// Original functions retain their names for comparison with build 12340.
// See docs/original-collision-reference.md. Explicit double intermediates and
// float stores preserve the original x87 spill boundaries; do not reassociate.
struct SurfaceTraversal {
    std::span<const MovementFacet> facets;
    SurfaceMovementState& state;
    float m_collisionRadius, m_collisionHeight, m_stepUpHeight;
    bool clientControlled;
    std::optional<uint64_t> transportRequest;
    std::optional<uint32_t> fallResetTime;
    RequestGeometry* geometry=nullptr;
    bool ValidateTestVsFacetQuery(const C3Vector& displacement) {
        if (!geometry) return true;
        const SurfaceMovementShape shape{m_collisionRadius,m_collisionHeight,m_stepUpHeight,clientControlled};
        auto expanded=expandMovementFacetRequest(geometry->cache.request,state,shape,displacement,geometry->local,geometry->ghost);
        if (!expanded) return true;
        const bool ready=geometry->acquire(*expanded);
        facets=geometry->cache.solids;
        return ready;
    }

    static constexpr float s_terminalVelocity = 60.148003f, s_safeFallTerminalVelocity = 7;
    static constexpr uint8_t s_volumeEdges[32] = {0,1,0,2,0,3,0,4,1,2,2,3,3,4,4,1,1,5,2,6,3,7,4,8,5,6,6,7,7,8,8,5};
    int DistanceToMove(const C3Vector& dir, float distance,
                       uint32_t* index, C4Plane* planes, uint32_t* count,
                       float* allowed) {
        *count=0;
        if (std::abs(distance)>=9.5367432e-7f &&
            !ValidateTestVsFacetQuery(dir*std::max(distance,.027777778f))) { *allowed=0;return 0; }
        const auto hit = traceMovementBody(facets,
            MovementBody(state.position, m_collisionRadius, m_collisionHeight), dir, distance);
        *index = hit.facet;
        *count = hit.planeCount;
        *allowed = hit.distance;
        std::copy_n(hit.planes.data(), hit.planeCount, planes);
        return 1;
    }
    MovementRequestResult CollideRequestMove(RequestGeometry& world, const C3Vector& move,
                                             uint32_t time,uint32_t elapsed) {
        MovementRequestResult result;
        world.events=&result.events;
        if (!elapsed) return result;
        const C3Vector target=state.position+move;
        const float margin=m_collisionRadius+71.375595f;
        const double mapX=17066.666f-double(target.x),mapY=17066.666f-double(target.y);
        if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z) ||
            mapX<margin || mapY<margin || mapX>=34133.332f-double(margin) || mapY>=34133.332f-double(margin)) {
            result.elapsedMs=elapsed;return result;
        }
        const double vertical=state.flags&(MOVEMENTFLAG_SWIMMING|MOVEMENTFLAG_FLYING)?move.z:0;
        const double length=std::sqrt((double(move.y)*move.y+double(move.x)*move.x)+vertical*vertical);
        const float speed=float(length/(double(elapsed)*.001f));
        C3Vector direction{0};
        if (std::abs(length)>=9.5367432e-7f)
            direction={float(move.x*(1.0/length)),float(move.y*(1.0/length)),float(vertical*(1.0/length))};
        uint32_t consumed=0;
        while ((state.flags&0x40c0100f) && !(state.flags&MOVEMENTFLAG_ROOT) && consumed<elapsed) {
            const uint32_t step=elapsed-consumed;
            const float distance=float(double(step)*.001f*speed);
            const auto request=GetMoveFacetRequest(direction,distance,step,world.local,world.ghost);
            const bool ready=world.acquire(request);
            facets=world.cache.solids;
            if (!ready) {
                result.events.push_back({MovementRequestEvent::Kind::SkipTime,step,0,0,0,false});
                state.anchorElapsedMs-=step;
                consumed=elapsed;
                break;
            }
            const uint32_t oldFlags=state.flags,oldFlags2=state.flags2;
            const bool jumping=(oldFlags&MOVEMENTFLAG_FALLING) && state.fallVelocity!=0;
            const float oldSpeed=state.currentSpeed;
            uint32_t used;
            if (state.flags&(MOVEMENTFLAG_SWIMMING|MOVEMENTFLAG_FLYING))
                used=Swim(world.cache.liquids,step,distance,direction);
            else if (state.flags&MOVEMENTFLAG_FALLING) used=Fall(time+consumed,step,distance,C2Vector(direction));
            else if (state.flags&0x40000000) used=HoverMove(time+consumed,step,distance,direction);
            else used=TraceSurface(step,distance,C2Vector(direction));
            consumed+=used;
            result.events.push_back({MovementRequestEvent::Kind::StateChanged,time+consumed,
                                    elapsed-consumed,oldFlags,oldFlags2,jumping});
            if (!(state.flags&MOVEMENTFLAG_FALLING) && (oldFlags&MOVEMENTFLAG_FALLING)) {
                const uint32_t unused=step-used;
                state.anchorElapsedMs=state.anchorElapsedMs>=unused ? state.anchorElapsedMs-unused : 0;
                break;
            }
            if (state.currentSpeed!=oldSpeed) { CalcCurrentSpeed(0);break; }
            if (!state.anchorElapsedMs && (state.flags&15)) state.anchorElapsedMs=elapsed-consumed;
        }
        state.groundNormal=queryMovementGroundNormal(facets,MovementBody(state.position,m_collisionRadius,m_collisionHeight));
        result.elapsedMs=consumed;
        result.geometryStatus=world.status;
        result.transportRequest=transportRequest;
        result.fallResetTime=fallResetTime;
        return result;
    }
    MovementFacetRequest BodyFacetRequest(bool local,bool ghost) const {
        MovementFacetRequest result;
        const auto position=state.position;
        result.bodyMin={position.x-m_collisionRadius,position.y-m_collisionRadius,position.z};
        result.bodyMax={position.x+m_collisionRadius,position.y+m_collisionRadius,position.z+m_collisionHeight};
        result.solidFlags=clientControlled ? 0x100111 : 0x102111;
        if (local) result.solidFlags|=0x80000000;
        if ((state.flags&0x10000000) && !(state.flags&MOVEMENTFLAG_SWIMMING) &&
            (state.pitch>-.6457718f || (state.flags2&0x200))) result.solidFlags|=0x10000;
        if (state.flags&MOVEMENTFLAG_FLYING) {
            result.solidFlags|=0x200;
            if (!(state.flags2&0x4000)) result.solidFlags|=0x20000;
        }
        if (ghost) result.solidFlags|=0x8000;
        result.queryLiquid=bool(state.flags&MOVEMENTFLAG_SWIMMING);
        result.queryMin=result.bodyMin;result.queryMax=result.bodyMax;
        return result;
    }
    MovementFacetRequest GetMoveFacetRequest(const C3Vector& direction, float distance,
                                             uint32_t elapsed, bool local, bool ghost) const {
        auto result=BodyFacetRequest(local,ghost);
        const auto position=state.position;
        auto& low=result.queryMin;
        auto& high=result.queryMax;
        low=result.bodyMin;high=result.bodyMax;
        auto extend=[&](const C3Vector& a,const C3Vector& b) {
            low=glm::min(low,a);high=glm::max(high,b);
        };
        C3Vector move=direction*distance;
        if (state.flags & MOVEMENTFLAG_FALLING) {
            const double vertical=double(move.z)-RelDistanceFallen(elapsed+state.fallTimeMs);
            const glm::dvec3 delta(move.x,move.y,vertical);
            extend(glm::vec3(glm::dvec3(low)+delta),glm::vec3(glm::dvec3(high)+delta));
            const double spread=double(float(vertical))*-1.1866661f;
            if (spread>0) {
                low.x=float(low.x-spread);low.y=float(low.y-spread);
                high.x=float(high.x+spread);high.y=float(high.y+spread);
            }
        } else if (state.flags & (MOVEMENTFLAG_SWIMMING|MOVEMENTFLAG_FLYING)) {
            const double half=double(distance)*.5f;
            glm::dvec3 center=glm::dvec3(direction)*half+glm::dvec3(position);
            center.y=float(center.y);center.z=float(center.z);
            const double radius=double(m_collisionRadius)*1.4142135f;
            const double height=std::max(radius,double(m_collisionHeight));
            extend({float(center.x-half-radius),float(center.y-half-radius),float(center.z-half)},
                   {float(center.x+half+radius),float(center.y+half+radius),float(half+center.z+height)});
        } else {
            double reach=float(m_collisionRadius+0.0013888889f);
            if (reach<double(GetStepUpHeight())*1.1917536f) reach=double(GetStepUpHeight())*1.1917536f;
            const double extent=reach+distance;
            const glm::dvec3 extra=glm::dvec3(direction)*extent;
            extend(glm::vec3(glm::dvec3(low)+extra),glm::vec3(glm::dvec3(high)+extra));
            const double half=double(distance)*.5f;
            glm::dvec3 center=glm::dvec3(direction)*half+glm::dvec3(position);
            center.y=float(center.y);center.z=float(center.z);
            const double radius=half+double(m_collisionRadius)*1.4142135f;
            extend({float(center.x-radius),float(center.y-radius),float(center.z)},
                   {float(center.x+radius),float(center.y+radius),float(center.z)});
            const float up=std::max(distance,GetStepUpHeight()+GetStepUpHeight());
            high.z=up+high.z;
            low.z=float(double(low.z)-(double(GetStepUpHeight())+double(distance)*1.1917536f));
        }
        for (int axis=0;axis<3;++axis) { low[axis]-=0.0013888889f;high[axis]+=0.0013888889f; }
        return result;
    }
    void IntersectPlanes(const C4Plane& a, const C4Plane& b, const C4Plane& c, C3Vector& point) {
        const auto matrix = glm::transpose(glm::mat3(a.normal,b.normal,c.normal));
        point = glm::inverse(matrix)*C3Vector(-a.offset,-b.offset,-c.offset);
    }
    void SetTransport(WOWGUID owner) { transportRequest = owner.m_guid; }
    // 0x0075e250
    C3Vector CalcRunWalkBlockingObstaclePush(const C3Vector& move, float dt, float maxDistance, const C3Vector& normal) const {
        if (normal.z < 0.0f && -normal.z > 0.64278764f) {
            return C3Vector(0.0f, 0.0f, 0.0f);
        }

        glm::dvec2 direction(normal);
        double lengthSq = direction.y * direction.y + direction.x * direction.x;

        if (lengthSq > 0.00000023841858f) {
            double scale = 1.0f / sqrt(lengthSq);
            direction.x = direction.x * scale;
            direction.y = direction.y * scale;
        }

        double push = (-double(normal.x) * move.x + (-double(normal.y) * move.y + -double(normal.z) * move.z)) * dt;
        double denominator = normal.y * direction.y + normal.x * direction.x + normal.z * 0.0f;
        double distance = fabs(denominator) < 0.00000023841858f ? push : push / denominator;
        float resultX = direction.x * (distance + 0.001f);
        float resultY = direction.y * (distance + 0.001f);

        C2Vector move2D(move);
        double moveX = double(move2D.x) * dt;
        double moveY = double(dt) * move2D.y;
        double totalX = moveX + resultX;
        double totalY = moveY + resultY;
        double totalSq = totalY * totalY + totalX * totalX;

        if (double(maxDistance) * maxDistance < totalSq) {
            double scale = maxDistance / sqrt(totalSq);
            resultX = totalX * scale - moveX;
            resultY = totalY * scale - moveY;
        }

        return C3Vector(resultX, resultY, 0.0f);
    }

    // 0x0075d740
    int32_t ComputeSteepSurfacePushNormalImpl(const C4Plane& plane) const {
        float top = m_collisionRadius * 1.849399f;
        C3Vector points[5];
        points[0] = state.position;
        points[1] = C3Vector(m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, state.position.z + top);
        points[2] = C3Vector(state.position.x - m_collisionRadius, m_collisionRadius + state.position.y, state.position.z + top);
        points[3] = C3Vector(m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, state.position.z + top);
        points[4] = C3Vector(-m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, top + state.position.z);

        for (uint32_t i = 0; i < 5; i++) {
            if (fabs(fabs(points[i].x * plane.normal.x + points[i].z * plane.normal.z + points[i].y * plane.normal.y + plane.offset)) < 0.0013888889f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075d890. The binary really takes fabs of both endpoint distances
    // before comparing their product with zero. Preserve this original
    // finite-input behaviour; replacing it with a signed-plane test changes
    // the falling-edge response.
    int32_t PlaneIntersectsVolume(const C4Plane& plane) const {
        float top = m_collisionRadius * 1.849399f;
        C3Vector points[9];
        points[0] = state.position;
        points[1] = C3Vector(m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, state.position.z + top);
        points[2] = C3Vector(-m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, state.position.z + top);
        points[3] = C3Vector(m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, state.position.z + top);
        points[4] = C3Vector(-m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, top + state.position.z);
        points[5] = C3Vector(m_collisionRadius + state.position.x, m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);
        points[6] = C3Vector(state.position.x - m_collisionRadius, m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);
        points[7] = C3Vector(m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);
        points[8] = C3Vector(-m_collisionRadius + state.position.x, -m_collisionRadius + state.position.y, m_collisionHeight + state.position.z);

        for (uint32_t i = 0; i < 32; i += 2) {
            const C3Vector& a = points[s_volumeEdges[i]];
            const C3Vector& b = points[s_volumeEdges[i + 1]];
            float distanceA = fabs(a.y * plane.normal.y + a.z * plane.normal.z + a.x * plane.normal.x + plane.offset);
            float distanceB = fabs(b.y * plane.normal.y + b.z * plane.normal.z + b.x * plane.normal.x + plane.offset);

            if (distanceA * distanceB < 0.0f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075db00
    C3Vector PushOffObstacleEdge(const C3Vector& point, const C3Vector& direction, uint32_t facet) const {
        const CFacet& data = facets[facet];

        C3Vector edge = data.vertices[1] - data.vertices[0];
        float lengthSq = edge.y * edge.y + edge.z * edge.z + edge.x * edge.x;

        if (lengthSq > 0.00000023841858f) {
            float scale = 1.0f / sqrt(lengthSq);
            edge.x = edge.x * scale;
            edge.y = edge.y * scale;
            edge.z = edge.z * scale;
        }

        float t = (point.z - data.vertices[0].z) * edge.z + (point.y - data.vertices[0].y) * edge.y + (point.x - data.vertices[0].x) * edge.x;
        float dz = point.z - (t * edge.z + data.vertices[0].z);
        float dy = point.y - (edge.y * t + data.vertices[0].y);
        float dx = point.x - (t * edge.x + data.vertices[0].x);
        float best = dx * dx + dy * dy + dz * dz;

        C3Vector edge1 = data.vertices[2] - data.vertices[1];
        lengthSq = edge1.z * edge1.z + edge1.y * edge1.y + edge1.x * edge1.x;

        if (lengthSq > 0.00000023841858f) {
            float scale = 1.0f / sqrt(lengthSq);
            edge1.x = edge1.x * scale;
            edge1.y = edge1.y * scale;
            edge1.z = scale * edge1.z;
        }

        t = (point.z - data.vertices[1].z) * edge1.z + (point.y - data.vertices[1].y) * edge1.y + (point.x - data.vertices[1].x) * edge1.x;
        dy = point.y - (edge1.y * t + data.vertices[1].y);
        dz = point.z - (edge1.z * t + data.vertices[1].z);
        dx = point.x - (edge1.x * t + data.vertices[1].x);
        float distance = dx * dx + (dz * dz + dy * dy);

        if (best > distance) {
            best = distance;
            edge = edge1;
        }

        C3Vector edge2 = data.vertices[0] - data.vertices[2];
        lengthSq = edge2.y * edge2.y + edge2.z * edge2.z + edge2.x * edge2.x;

        if (lengthSq > 0.00000023841858f) {
            float scale = 1.0f / sqrt(lengthSq);
            edge2.x = edge2.x * scale;
            edge2.y = edge2.y * scale;
            edge2.z = scale * edge2.z;
        }

        t = (point.z - data.vertices[2].z) * edge2.z + (point.y - data.vertices[2].y) * edge2.y + (point.x - data.vertices[2].x) * edge2.x;
        dx = point.x - (edge2.x * t + data.vertices[2].x);
        dy = point.y - (edge2.y * t + data.vertices[2].y);
        dz = point.z - (edge2.z * t + data.vertices[2].z);

        if (dx * dx + dz * dz + dy * dy < best) {
            edge = edge2;
        }

        C3Vector push(data.plane.normal.z * edge.y - data.plane.normal.y * edge.z, edge.z * data.plane.normal.x - data.plane.normal.z * edge.x, edge.x * data.plane.normal.y - edge.y * data.plane.normal.x);

        if (direction.z * push.z + direction.y * push.y + push.x * direction.x < 0.0f) {
            push = C3Vector(-push.x, -push.y, -push.z);
        }

        return push;
    }

    // 0x0075cfe0
    int32_t IsFacetOverhead(uint32_t facet) const {
        float left = state.position.x - m_collisionRadius;
        float right = m_collisionRadius + state.position.x;
        float front = state.position.y - m_collisionRadius;
        float back = m_collisionRadius + state.position.y;
        float top = m_collisionHeight + state.position.z;
        C3Vector corners[4] = { C3Vector(left, front, top), C3Vector(right, front, top), C3Vector(left, back, top), C3Vector(right, back, top) };
        const CFacet& data = facets[facet];

        for (uint32_t i = 0; i < 4; i++) {
            if (fabs(double(corners[i].z) * data.plane.normal.z + double(corners[i].x) * data.plane.normal.x + double(corners[i].y) * data.plane.normal.y + data.plane.offset) < 0.0013888889f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075d2d0
    int32_t IsFacetWall(uint32_t facet) const {
        const CFacet& data = facets[facet];

        if (data.plane.normal.z > 0.64278764f) {
            return 0;
        }

        return data.plane.normal.z >= 0.0f || -data.plane.normal.z <= 0.64278764f;
    }

    // 0x006e9520
    double GetRemainingStepUpHeight() const {
        double height = clientControlled ? m_stepUpHeight : 2.0f;

        if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
            height -= double(state.position.z) - state.stepStartZ;
        }

        if (height <= 0.0f) {
            return 0.0f;
        }

        return height;
    }

    // 0x006e8fc0
    float GetStepUpHeight() const {
        if (!clientControlled) {
            return 2.0f;
        }

        return m_stepUpHeight;
    }

    // 0x0075b4f0
    void RestoreMoveState(const SurfaceMovementState& saved) {
        state.anchorPosition = saved.anchorPosition;
        state.anchorFacing = saved.anchorFacing;
        state.anchorPitch = saved.anchorPitch;
        state.anchorElapsedMs = saved.anchorElapsedMs;
        state.fallTimeMs = saved.fallTimeMs;
        state.direction = saved.direction;
        state.direction2D = saved.direction2D;
        state.flags = saved.flags;
        state.currentSpeed = saved.currentSpeed;
    }

    // 0x0075b480
    void SaveMoveState(SurfaceMovementState& saved) const {
        saved.anchorPosition = state.anchorPosition;
        saved.anchorFacing = state.anchorFacing;
        saved.anchorPitch = state.anchorPitch;
        saved.anchorElapsedMs = state.anchorElapsedMs;
        saved.fallTimeMs = state.fallTimeMs;
        saved.direction = state.direction;
        saved.direction2D = state.direction2D;
        saved.flags = state.flags;
        saved.currentSpeed = state.currentSpeed;
    }

    // 0x0075e040
    int32_t CheckFallImpactThreshold(float timeFallen, float elapsed, const C3Vector& move, int32_t force) const {
        if (force) {
            return 1;
        }

        if (state.fallVelocity < 0.0f) {
            float peak = TimeToJumpPeak();

            if (elapsed + timeFallen < peak) {
                return 1;
            }

            if (timeFallen > peak) {
                return 0;
            }

            float distance = (peak - timeFallen) * state.currentSpeed;

            if (move.x * move.x + move.y * move.y < distance * distance) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075eb00
    double GetTimeJustFallen(float elapsed, float timeFallen, C3Vector* move, float* distance, int32_t force, float speed) {
        int32_t impact = CheckFallImpactThreshold(timeFallen, elapsed, *move, force);
        float moveTime = 0.0f;

        if (speed > 9.5367432e-7f) {
            C2Vector move2D(*move);
            moveTime = sqrt(double(move2D.x) * move2D.x + double(move2D.y) * move2D.y) / speed * elapsed;
        }

        double fallTime = CalcTimeFallen(float(double(state.fallStartZ) - state.position.z - move->z), impact);

        if (!(timeFallen < fallTime)) {
            move->x = 0.0f;
            move->y = 0.0f;
            move->z = 0.0f;
            *distance = 0.0f;
            return 0.0f;
        }

        double remaining = fallTime - timeFallen;

        if (elapsed < remaining) {
            return elapsed;
        }

        if (moveTime > remaining) {
            double scale = remaining / moveTime;
            const double x = move->x * scale, y = scale * move->y;
            move->x = float(x);
            move->y = float(y);
            *distance = float(sqrt((x*x+y*y)+double(move->z)*move->z));
        }

        return remaining;
    }

    // 0x0075e0c0
    C3Vector CalcRunWalkWalkableObstaclePush(const C3Vector& move, float* scale, C3Vector normal, int32_t useAltNormal, const C3Vector& altNormal) const {
        if (useAltNormal && normal.z <= 0.64278764f && !(fabs(altNormal.z * altNormal.z + altNormal.y * altNormal.y + altNormal.x * altNormal.x) < 2.3841858e-7f)) {
            normal.x = -altNormal.x;
            normal.y = -altNormal.y;
            normal.z = -altNormal.z;
        }

        double push = (-double(normal.x) * move.x + (-double(normal.y) * move.y + -double(normal.z) * move.z)) * *scale;
        float height;

        if (fabs(normal.z) < 2.3841858e-7f) {
            height = push >= 0.0f ? 3.4028235e38f : -3.4028235e38f;
        } else {
            height = push / normal.z;
        }

        float result;

        if ((state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) && height < 0.0f) {
            *scale = 0.0f;
            result = GetRemainingStepUpHeight();
        } else if (GetRemainingStepUpHeight() >= height) {
            result = height;

            if (height < -GetStepUpHeight()) {
                *scale = -(GetStepUpHeight() * (double(*scale) / height));
                result = -GetStepUpHeight();
            }
        } else {
            *scale = GetRemainingStepUpHeight() * (double(*scale) / height);
            result = GetRemainingStepUpHeight();
        }

        return C3Vector(0.0f * result, 0.0f * result, result);
    }

    // 0x0075b690
    int32_t IsSurfaceTooSteep(uint32_t facet) const {
        return facets[facet].plane.normal.z <= 0.64278764f;
    }

    // 0x0075d0a0
    int32_t WalkableFacetEnclosesPoint(uint32_t facet, const C3Vector& point) const {
        C4Plane planes[4];

        for (uint32_t i = 0; i < 4; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        C3Vector up(0.0f, 0.0f, 1.0f);
        uint8_t indices[3] = { 0, 1, 2 };
        const CFacet& data = facets[facet];
        // The original consumes the initialized planes even if extrusion stops
        // early on a degenerate edge. Keep the same footprint semantics.
        (void)detail::extrudeMovementFace(data.vertices, indices, up, planes);

        for (uint32_t i = 0; i < 3; i++) {
            if (double(planes[i].normal.z) * point.z + double(planes[i].normal.x) * point.x + double(planes[i].normal.y) * point.y + planes[i].offset > 0.083333336f) {
                return 0;
            }
        }

        return 1;
    }

    // 0x0075d340
    int32_t IsSlopeFallable(uint32_t facet, const C3Vector& move) const {
        C3Vector point(state.position.x + move.x, state.position.y + move.y, state.position.z + move.z);
        float threshold = clientControlled ? 0.64278764f : 0.17364818f;
        return threshold < facets[facet].plane.normal.z && WalkableFacetEnclosesPoint(facet, point);
    }

    // 0x0075d4b0
    void ComputeSteepSurfacePushNormalHelper(uint32_t facet, const C3Vector& direction, C3Vector& edge, const C3Vector& point) {
        uint32_t next[3] = { 1, 2, 0 };
        float best = 3.4028235e38f;
        edge = C3Vector(0.0f, 0.0f, 0.0f);
        const CFacet& data = facets[facet];

        for (uint32_t i = 0; i < 3; i++) {
            const C3Vector& v = data.vertices[i];
            const C3Vector& w = data.vertices[next[i]];
            double ex = double(v.x) - w.x;
            double ey = double(v.y) - w.y;
            double ez = double(v.z) - w.z;
            double length = sqrt(ez * ez + ey * ey + ex * ex);

            if (fabs(length) < 0.00000023841858f) {
                continue;
            }

            double scale = 1.0 / length;
            glm::dvec3 n(ex * scale, ey * scale, scale * ez);
            double distSq;

            if (fabs(direction.z * n.z + direction.y * n.y + direction.x * n.x - 1.0f) >= 0.00000095367432f) {
                double cx = direction.z * n.y - direction.y * n.z;
                double cy = n.z * direction.x - direction.z * n.x;
                double cz = n.x * direction.y - n.y * direction.x;
                double d = -(point.z * cz + point.y * cy + point.x * cx);
                double dist = cz * v.z + (cx * v.x + cy * v.y) + d;
                distSq = dist * dist;
            } else {
                double t = ((double(v.z) - point.z) * direction.z + (double(v.y) - point.y) * direction.y) + (double(v.x) - point.x) * direction.x;
                double dx = v.x - (direction.x * t + point.x);
                double dy = v.y - (direction.y * t + point.y);
                double dz = v.z - (t * direction.z + point.z);
                distSq = dz * dz + dy * dy + dx * dx;
            }

            if (distSq < best) {
                best = distSq;
                edge = n;
            }
        }
    }

    // 0x0075de40
    int32_t CheckFallingConditions(const C4Plane* planes, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            if (fabs(planes[i].normal.z - 1.0f) < 0.00000095367432f) {
                return 1;
            }
        }

        return 0;
    }

    // 0x0075de80
    int32_t GetBoxPushNormal(const C4Plane* planes, uint32_t count, C3Vector& normal) {
        if (!count || count > 4) {
            return 0;
        }

        for (uint32_t i = 0; i < count; i++) {
            if (fabs(planes[i].normal.z - -0.4756366f) >= 0.00000095367432f) {
                return 0;
            }
        }

        switch (count) {
            case 1:
                normal = planes[0].normal;
                break;

            case 2:
                normal.x = (planes[1].normal.x + planes[0].normal.x) * 0.638556f;
                normal.y = (planes[1].normal.y + planes[0].normal.y) * 0.638556f;
                normal.z = 0.638556f * (planes[1].normal.z + planes[0].normal.z);
                break;

            case 3: {
                uint32_t flatY[3];
                uint32_t flatX[3];
                uint32_t flatYCount = 0;
                uint32_t flatXCount = 0;

                for (uint32_t i = 0; i < 3; i++) {
                    if (fabs(planes[i].normal.y) < 0.00000095367432f) {
                        flatY[flatYCount++] = i;
                    } else if (fabs(planes[i].normal.x) < 0.00000095367432f) {
                        flatX[flatXCount++] = i;
                    }
                }

                normal = planes[flatYCount == 1 ? flatY[0] : flatX[0]].normal;
                break;
            }

            case 4:
                normal.x = 0.0f;
                normal.y = 0.0f;
                normal.z = 0.0f;
                break;
        }

        return 1;
    }

    // 0x0075e760
    C3Vector GetSteepSurfacePushNormal(uint32_t facet, const C4Plane* planes, uint32_t count) {
        if (count == 1) {
            if (PlaneIntersectsVolume(facets[facet].plane)) {
                return C3Vector(-planes[0].normal.x, -planes[0].normal.y, -planes[0].normal.z);
            }
        } else if (count == 2) {
            double x = double(planes[1].normal.z) * planes[0].normal.y - double(planes[0].normal.z) * planes[1].normal.y;
            double y = double(planes[1].normal.x) * planes[0].normal.z - double(planes[0].normal.x) * planes[1].normal.z;
            double z = double(planes[0].normal.x) * planes[1].normal.y - double(planes[1].normal.x) * planes[0].normal.y;
            double scale = 1.0 / sqrt(y * y + x * x + z * z);
            x *= scale; y *= scale; z *= scale;
            C3Vector direction(x, y, z);
            const CFacet& data = facets[facet];

            if (fabs(z) >= 0.00000095367432f && fnotequal(float(y * data.plane.normal.y + z * data.plane.normal.z + x * data.plane.normal.x), 0.0f) && (fabs(double(direction.z) - 1.0f) < 0.00000095367432f || !ComputeSteepSurfacePushNormalImpl(data.plane))) {
                C3Vector point{0};
                IntersectPlanes(facets[facet].plane, planes[0], planes[1], point);
                C3Vector edge{0};
                ComputeSteepSurfacePushNormalHelper(facet, direction, edge, point);
                C3Vector normal = glm::cross(glm::dvec3(direction), glm::dvec3(edge));
                float length = sqrt(double(normal.y) * normal.y + double(normal.z) * normal.z + double(normal.x) * normal.x);

                if (fequal(length, 0.0f)) {
                    return -planes[0].normal;
                }

                normal = glm::dvec3(normal) * (1.0 / length);

                if (double(planes[0].normal.x) * normal.x + double(normal.y) * planes[0].normal.y + double(planes[0].normal.z) * normal.z <= 0.0f) {
                    return normal;
                }

                return C3Vector(-normal.x, -normal.y, -normal.z);
            }
        }

        return facets[facet].plane.normal;
    }

    // 0x0075e9c0
    C2Vector CalcFallObstaclePush(const C3Vector& direction, float distance, float maxDistance, uint32_t facet, const C4Plane* planes, uint32_t count) {
        C3Vector normal{0};

        if (IsFacetWall(facet)) {
            normal = GetSteepSurfacePushNormal(facet, planes, count);
        } else {
            C3Vector point(double(direction.x) * distance + state.position.x, double(direction.y) * distance + state.position.y, double(distance) * direction.z + state.position.z);
            normal = PushOffObstacleEdge(point, direction, facet);
        }

        glm::dvec2 push(normal);
        double lengthSq = push.y * push.y + push.x * push.x;

        if (lengthSq > 0.00000023841858f) {
            double scale = 1.0f / sqrt(lengthSq);
            push.x = push.x * scale;
            push.y = push.y * scale;
        }

        double d = (-double(normal.x) * direction.x + (-double(normal.y) * direction.y + -double(normal.z) * direction.z)) * (double(maxDistance) - distance);
        double denom = normal.x * push.x + normal.y * push.y + normal.z * 0.0f;
        double scale = fabs(denom) < 0.00000023841858f ? d : d / denom;
        return C2Vector(push.x * (scale + 0.001f), push.y * (scale + 0.001f));
    }

    // 0x0075d1c0
    uint32_t UseWalkableRedirection(uint32_t facet, int32_t* cleared) {
        if (cleared) {
            *cleared = 0;
        }

        float threshold = clientControlled ? 0.64278764f : 0.17364818f;

        if (threshold < facets[facet].plane.normal.z) {
            if (WalkableFacetEnclosesPoint(facet, state.position)) {
                state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            }

            return 1;
        }

        if (facets[facet].plane.normal.z < 0.0f && IsFacetOverhead(facet)) {
            if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
                if (cleared) {
                    *cleared = 1;
                }

                state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            }

            return -facets[facet].plane.normal.z > 0.64278764f;
        }

        return state.flags & MOVEMENTFLAG_SPLINE_ELEVATION;
    }

    // 0x00760fc0
    int32_t TryFallingDown(float timeFallen, C3Vector* move, float* distance, C2Vector* push, uint64_t* facetId, int32_t* onSlope, int32_t* landed) {
        *landed = 0;
        *onSlope = 0;
        float length = *distance;

        if (fabs(length) < 0.00000023841858f) {
            *facetId = 0;
            return 0;
        }

        C4Plane planes[9];

        for (uint32_t i = 0; i < 7; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        C3Vector direction(move->x * (1.0 / length), move->y * (1.0 / length), 1.0 / length * move->z);
        uint32_t facetIndex;
        uint32_t planeCount;

        if (!DistanceToMove(direction, length, &facetIndex, planes, &planeCount, distance)) {
            move->x = 0.0f;
            move->y = 0.0f;
            move->z = 0.0f;
            return 2;
        }

        if (facetIndex == facets.size()) {
            *facetId = 0;
            return 0;
        }

        float d = *distance;
        move->x = direction.x * d;
        move->y = direction.y * d;
        move->z = d * direction.z;
        *facetId = facets[facetIndex].owner;

        if (IsSlopeFallable(facetIndex, *move)) {
            *onSlope = 1;
            return 1;
        }

        if (direction.z > 0.0f) {
            double peak = TimeToJumpPeak() - timeFallen;

            if (peak >= 0.0f && peak * state.currentSpeed >= *distance && CheckFallingConditions(planes, planeCount)) {
                *landed = 1;
                *facetId = 0;
                return 1;
            }
        }

        *push = CalcFallObstaclePush(direction, *distance, length, facetIndex, planes, planeCount);
        return 1;
    }

    int32_t StartMove(int32_t forward, int32_t force) {
        state.flags &= ~(MOVEMENTFLAG_PENDING_STOP | MOVEMENTFLAG_PENDING_FORWARD | MOVEMENTFLAG_PENDING_BACKWARD);
        uint32_t flags = state.flags;
        int32_t falling = 0;
    
        if (!force && (flags & MOVEMENTFLAG_FALLING)) {
            if (flags & (MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT)) {
                if (forward) {
                    if (!(flags & MOVEMENTFLAG_FORWARD)) {
                        state.flags = flags | MOVEMENTFLAG_PENDING_FORWARD;
                        return 0;
                    }
                } else if (!(flags & MOVEMENTFLAG_BACKWARD)) {
                    state.flags = flags | MOVEMENTFLAG_PENDING_BACKWARD;
                }
    
                return 0;
            }
    
            falling = 1;
        }
    
        if (forward) {
            state.flags = (flags & ~(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD)) | MOVEMENTFLAG_FORWARD;
        } else {
            state.flags = (flags & ~(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD)) | MOVEMENTFLAG_BACKWARD;
        }
    
        UpdateAnchors(falling);
        CalcCurrentSpeed(falling);
        return 1;
    }

    int32_t StartStrafe(int32_t left) {
        state.flags &= ~(MOVEMENTFLAG_PENDING_STRAFE_STOP | MOVEMENTFLAG_PENDING_STRAFE_LEFT | MOVEMENTFLAG_PENDING_STRAFE_RIGHT);
        uint32_t flags = state.flags;
    
        if (state.flags2 & MOVEMENTFLAG2_NO_STRAFE) {
            return 0;
        }
    
        int32_t force = 0;
    
        if (flags & MOVEMENTFLAG_FALLING) {
            if (flags & (MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT)) {
                if (left) {
                    if (!(flags & MOVEMENTFLAG_STRAFE_LEFT)) {
                        state.flags = flags | MOVEMENTFLAG_PENDING_STRAFE_LEFT;
                        return 0;
                    }
                } else if (!(flags & MOVEMENTFLAG_STRAFE_RIGHT)) {
                    state.flags = flags | MOVEMENTFLAG_PENDING_STRAFE_RIGHT;
                }
    
                return 0;
            }
    
            force = 1;
        }
    
        state.flags = (flags & ~(MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT)) | (left ? MOVEMENTFLAG_STRAFE_LEFT : MOVEMENTFLAG_STRAFE_RIGHT);
        UpdateAnchors(force);
        CalcCurrentSpeed(force);
        return 1;
    }

    // 0x006eb3b0, state transitions for a mover with no active spline.
    // Active input/queue notification belongs to the caller's event adapter.
    bool HandlePendingActions() {
        const uint32_t oldFlags = state.flags;
        if (oldFlags & MOVEMENTFLAG_PENDING_STOP) {
            state.flags &= ~(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_PENDING_STOP);
            CalcCurrentSpeed(0);
            UpdateAnchors(0);
        }
        if (state.flags & MOVEMENTFLAG_PENDING_STRAFE_STOP) {
            state.flags &= ~(MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT | MOVEMENTFLAG_PENDING_STRAFE_STOP);
            CalcCurrentSpeed(0);
            UpdateAnchors(0);
        }
        if (state.flags & (MOVEMENTFLAG_PENDING_FORWARD | MOVEMENTFLAG_PENDING_BACKWARD))
            StartMove(state.flags & MOVEMENTFLAG_PENDING_FORWARD, 0);
        if (state.flags & (MOVEMENTFLAG_PENDING_STRAFE_LEFT | MOVEMENTFLAG_PENDING_STRAFE_RIGHT))
            StartStrafe(state.flags & MOVEMENTFLAG_PENDING_STRAFE_LEFT);
        state.flags &= ~(MOVEMENTFLAG_PENDING_STOP | MOVEMENTFLAG_PENDING_STRAFE_STOP |
            MOVEMENTFLAG_PENDING_FORWARD | MOVEMENTFLAG_PENDING_BACKWARD |
            MOVEMENTFLAG_PENDING_STRAFE_LEFT | MOVEMENTFLAG_PENDING_STRAFE_RIGHT | MOVEMENTFLAG_PENDING_ROOT);
        return (oldFlags ^ state.flags) & 15;
    }

    bool IsGravityDisabled() const {
        return (state.flags2 & 4) || (state.flags & 0x400);
    }

    // 0x009883f0, without an active spline.
    void JumpFromWater() {
        if ((state.flags & 0x40000000) && !(state.flags & MOVEMENTFLAG_SWIMMING)) return;
        if (state.flags & (MOVEMENTFLAG_ROOT | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FLYING)) return;
        if (!(state.flags2 & 4) && IsGravityDisabled()) return;
        if (state.flags2 & 2) return;
        StartFalling(state.flags & MOVEMENTFLAG_SWIMMING ? -9.0967484f : -7.9555473f);
    }

    // 0x00760b40. The solid hit latches for this request: after touching a
    // wall, subsequent iterations no longer query the liquid surface.
    uint32_t Swim(std::span<const MovementFacet> liquids, uint32_t elapsed,
                  float distance, C3Vector direction) {
        if (!elapsed) return 0;
        if (fabs(distance) < 0.00000095367432f) return elapsed;
        const float total = float(elapsed);
        float consumed = 0;
        float remaining = distance;
        uint32_t stuck = 0;
        bool hitSolid = false, moved = false;
        bool queryReady=true;
        auto query = [&](bool liquid) {
            queryReady=ValidateTestVsFacetQuery(direction*std::max(remaining,.027777778f));
            if (geometry) liquids=geometry->cache.liquids;
            const auto input=liquid?liquids:facets;
            const float height = liquid && (state.flags & MOVEMENTFLAG_SWIMMING)
                               ? m_collisionHeight * 0.75f : m_collisionHeight;
            return traceMovementBody(input, MovementBody(state.position, m_collisionRadius, height), direction, remaining);
        };
        auto hit = query(false);
        if (!queryReady) return elapsed;
        while (true) {
            auto input = facets;
            if (hit.facet < facets.size()) hitSolid = true;
            else {
                if (!hitSolid && !IsGravityDisabled()) {
                    input = liquids;
                    hit = query(true);
                    if (!queryReady) return elapsed;
                    input=liquids;
                }
                if (hit.facet == input.size()) {
                    state.position += direction * hit.distance;
                    break;
                }
            }
            state.position = C3Vector(glm::dvec3(direction) * double(hit.distance) + glm::dvec3(state.position));
            double progress = (double(total) - consumed) * (double(hit.distance) / remaining);
            double left = double(remaining) - hit.distance;
            double used = consumed;
            if (progress + 0.00000095367432f <= 1.0f) {
                if (++stuck > 5) { UpdateAnchors(0); return elapsed; }
            } else {
                stuck = 1;
                used += progress;
                consumed = float(used);
            }
            if (!hitSolid && (state.flags & MOVEMENTFLAG_ASCENDING)) {
                if (fnotequal(total, float(used))) moved = true;
                JumpFromWater();
                break;
            }
            if (total - used < 1.0f) {
                if (fnotequal(total, float(used))) moved = true;
                break;
            }
            const auto& n = input[hit.facet].plane.normal;
            moved = true;
            double push = (-double(n.y) * direction.y + -double(n.z) * direction.z + -double(n.x) * direction.x) * left + 0.001f;
            double x = direction.x * left + n.x * push;
            double y = n.y * push + direction.y * left;
            double z = n.z * push + left * direction.z;
            direction = C3Vector(x, y, z);
            double length = sqrt(y * y + z * z + x * x);
            remaining = float(length);
            if (fabs(length) < 0.00000095367432f) break;
            double scale = 1.0 / length;
            direction = C3Vector(x * scale, double(direction.y) * scale, double(direction.z) * scale);
            hit = query(false);
            if (!queryReady) return elapsed;
        }
        if (moved) UpdateAnchors(0);
        return elapsed;
    }

    int32_t TryStartFalling() {
        if (IsGravityDisabled() || (state.flags & (MOVEMENTFLAG_FALLING | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING))) return 0;
        return StartFalling(0);
    }

    // 0x0075f520, with resident geometry. Only lower body faces participate.
    bool GetCurrentHoverHeight(float* height, int32_t* noGround, uint64_t* owner) {
        const float distance = state.hoverHeight + state.hoverHeight;
        if (!ValidateTestVsFacetQuery({0,0,-distance})) { *height=0;return false; }
        auto hit = detail::traceMovementPyramid(facets,
            MovementBody(state.position, m_collisionRadius, m_collisionHeight), C3Vector(0,0,-1), distance);
        *height = hit.distance;
        *noGround = hit.facet == facets.size();
        *owner = *noGround ? 0 : facets[hit.facet].owner;
        if (!*noGround && state.hoverHeight > *height &&
            !(facets[hit.facet].plane.normal.z > (clientControlled ? 0.64278764f : 0.17364818f)))
            *height = state.hoverHeight;
        if (*height < 0.0013888889f) *height = 0;
        return true;
    }

    // 0x00762980
    uint32_t HoverMove(uint32_t time, uint32_t elapsed, float distance, C3Vector direction) {
        if (!elapsed) {
            return 0;
        }

        float hoverHeight;
        int32_t noGround;
        uint64_t facetId;

        if (!GetCurrentHoverHeight(&hoverHeight, &noGround, &facetId)) {
            return elapsed;
        }

        if (noGround && TryStartFalling()) {
            uint32_t result = Fall(time, elapsed, distance, C2Vector(direction));

            if (result) {
                return result;
            }
        }

        float total = static_cast<float>(elapsed);
        double climb = double(elapsed) * 0.001f * 3.5f;
        double mx = double(direction.x) * distance;
        double my = double(direction.y) * distance;
        double mz = double(distance) * direction.z;
        double delta = double(state.hoverHeight) - hoverHeight;

        if (-climb <= delta) {
            if (delta < climb) {
                climb = delta;
            }
        } else {
            climb = -climb;
        }

        mz = mz + climb;
        double length = sqrt(my * my + mx * mx + mz * mz);
        distance = float(length);

        if (fabs(length) >= 0.00000023841858f) {
            double scale = 1.0 / length;
            direction.x = mx * scale;
            direction.y = my * scale;
            direction.z = mz * scale;
        }

        if (fabs(length) < 0.00000095367432f) {
            return elapsed;
        }

        C4Plane planes[9];

        for (uint32_t i = 0; i < 7; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        uint32_t stuck = 0;
        float consumed = 0.0f;
        int32_t moved = 0;
        uint32_t facetIndex;
        uint32_t planeCount;
        float moveDist;

        if (!DistanceToMove(direction, distance, &facetIndex, planes, &planeCount, &moveDist)) {
            return elapsed;
        }

        while (true) {
            if (facetIndex >= facets.size()) {
                C3Vector move(direction.x * moveDist, direction.y * moveDist, moveDist * direction.z);
                state.position += move;
                goto done;
            }

            {
                if (facets[facetIndex].owner) {
                    facetId = facets[facetIndex].owner;
                }

                state.position.x = double(direction.x) * moveDist + state.position.x;
                state.position.y = double(direction.y) * moveDist + state.position.y;
                state.position.z = double(direction.z) * moveDist + state.position.z;
                double left = double(distance) - moveDist;
                double progress = (double(total) - consumed) * (double(moveDist) / distance);
                double used = consumed;
                distance = left;

                if (progress + 0.00000095367432f <= 1.0f) {
                    if (++stuck > 5) {
                        goto anchors;
                    }
                } else {
                    stuck = 1;
                    used += progress;
                    consumed = float(used);
                }

                if (total - used < 1.0f) {
                    if (fnotequal(total, float(used))) {
                        goto anchors;
                    }

                    goto done;
                }

                C3Vector slide(direction.x * left, direction.y * left, left * direction.z);
                C3Vector normal = facets[facetIndex].plane.normal;
                C3Vector push;

                if (UseWalkableRedirection(facetIndex, nullptr)) {
                    const CFacet& facet = facets[facetIndex];
                    double p = (-double(facet.plane.normal.y) * direction.y + -double(facet.plane.normal.z) * direction.z + -double(facet.plane.normal.x) * direction.x) * distance + 0.001f;
                    push = C3Vector(facet.plane.normal.x * p, facet.plane.normal.y * p, p * facet.plane.normal.z);
                } else {
                    push = CalcRunWalkBlockingObstaclePush(direction, left, sqrt(double(slide.y) * slide.y + double(slide.x) * slide.x), normal);
                }

                moved = 1;
                double x = double(slide.x) + push.x;
                double y = double(slide.y) + push.y;
                double z = double(slide.z) + push.z;
                direction = C3Vector(x, y, z);
                length = sqrt(y * y + z * z + x * x);
                distance = float(length);

                if (fabs(length) < 0.00000095367432f) {
                    goto anchors;
                }

                double scale = 1.0 / length;
                direction.x = x * scale;
                direction.y = scale * direction.y;
                direction.z = scale * direction.z;

                if (!DistanceToMove(direction, distance, &facetIndex, planes, &planeCount, &moveDist)) {
                    return elapsed;
                }
            }
        }

    done:
        if (!moved) {
            goto transport;
        }

    anchors:
        UpdateAnchors(0);

    transport:
        WOWGUID guid;
        guid.m_guid = facetId;
        SetTransport(guid);
        return elapsed;
    }


    // 0x007612b0
    uint32_t FallDown(uint32_t elapsed, C3Vector move, bool notify = false, uint32_t time = 0) {
        float timeFallen = 0.0f;
        float totalTime = float(double(elapsed) * 0.001f);
        float startTime = float(0.001f * double(state.fallTimeMs));
        int32_t stuck = 0;
        uint64_t transport = 0;
        C2Vector push(0.0f, 0.0f);
        int32_t slid = 0;
        int32_t flattened = 0;
        C2Vector move2D(move);
        const auto savedDir2D = state.direction2D;
        const auto savedDir = state.direction;
        const auto savedSpeed = state.currentSpeed;
        int32_t landed = 0;
        double length = float(sqrt(double(move.y) * move.y + double(move.x) * move.x + double(move.z) * move.z));
        float length2D = sqrt(double(move2D.y) * move2D.y + double(move2D.x) * move2D.x);
        float vertical = fabs(move.z);
        C2Vector start(state.position);
        int32_t iterations = 0;
        float startLength2D = length2D;
        double endTime;
        int32_t onSlope;

        while (true) {
            if (fabs(length) < 0.00000095367432f) {
                endTime = totalTime;
                goto finish;
            }

            {
                float distance = length;
                double scale = 1.0 / length;
                C3Vector direction(move.x * scale, move.y * scale, move.z * scale);
                float fallTime = startTime + timeFallen;
                int32_t result = TryFallingDown(fallTime, &move, &distance, &push, &transport, &onSlope, &landed);
                if (result==2) {
                    const uint32_t skip=uint32_t(std::nearbyint(float((double(totalTime)-timeFallen)*1000.0f)));
                    state.anchorElapsedMs-=skip;
                    state.fallTimeMs-=skip;
                    if (geometry && geometry->events)
                        geometry->events->push_back({MovementRequestEvent::Kind::SkipTime,skip,0,0,0,false});
                    endTime=totalTime;
                    goto finish;
                }

                double dt = result ? GetTimeJustFallen(totalTime - timeFallen, fallTime, &move, &distance, landed, length2D) : double(totalTime) - timeFallen;
                state.position.x = state.position.x + move.x;
                state.position.y = state.position.y + move.y;
                state.position.z = state.position.z + move.z;
                const double accumulated = timeFallen + dt;
                timeFallen = float(accumulated);

                if (landed) {
                    state.flags |= MOVEMENTFLAG_FALLING_FAR;
                    endTime = accumulated;
                    state.fallTimeMs = 0;
                    state.fallVelocity = 0.0f;
                    state.fallStartZ = state.position.z;
                    goto finish;
                }

                if (onSlope) {
                    goto stop;
                }

                if (accumulated + 0.00050000002f >= totalTime) {
                    endTime = totalTime;
                    goto finish;
                }

                slid = 1;
                double left = double(float(length)) - distance;

                if (dt > 0.00050000002f) {
                    stuck = 1;
                } else if (++stuck > 5) {
                    if (flattened || !fnotequal(length2D, 0.0f)) {
                        goto stop;
                    }

                    stuck = 0;
                    iterations++;
                    state.currentSpeed = 0.0f;
                    flattened = 1;
                    move.x = 0.0f;
                    move.y = 0.0f;
                    move.z = direction.z * left;
                    length = fabs(move.z);
                    continue;
                }

                move.x = direction.x * left + push.x;
                move.y = direction.y * left + push.y;
                move.z = left * direction.z;

                if (iterations) {
                    C2Vector position(state.position);
                    float tx = position.x - start.x;
                    float ty = position.y - start.y;
                    C2Vector next(move);
                    float dy = next.y + ty;
                    float dx = next.x + tx;
                    float distSq = dx * dx + dy * dy;

                    if ((startLength2D + 0.00000095367432f) * (startLength2D + 0.00000095367432f) < distSq && vertical * 1.1866661f * (vertical * 1.1866661f) < distSq) {
                        goto stop;
                    }
                }

                move2D = C2Vector(move);
                const double flatLength = sqrt(double(move2D.y) * move2D.y + double(move2D.x) * move2D.x);
                length2D = float(flatLength);

                if (fabs(flatLength) < 0.00000095367432f) {
                    state.currentSpeed = 0.0f;
                } else {
                    double inverse = 1.0 / flatLength;
                    const double x = move2D.x * inverse, y = move2D.y * inverse;
                    move2D = C2Vector(x,y);
                    double speed = (x * state.direction2D.x + y * state.direction2D.y) * state.currentSpeed;
                    state.currentSpeed = speed;

                    if (!(speed > 0.0f) && !(0.0f == speed)) {
                        state.currentSpeed = 0.0f;
                    }
                }

                iterations++;
                state.direction2D = move2D;
                length = sqrt(double(move.y) * move.y + double(move.x) * move.x + double(move.z) * move.z);
                state.direction = C3Vector(state.direction2D.x, state.direction2D.y, 0.0f);
            }
        }

    stop:
        StopFalling();
        endTime = timeFallen;

    finish:
        uint32_t used = static_cast<uint32_t>(std::nearbyint(float(endTime * 1000.0f)));

        if (!landed) {
            state.fallTimeMs += used;
        }

        if (notify) {
            if (landed) fallResetTime = used + time;
            const uint32_t flags = state.flags;
            if (flags & MOVEMENTFLAG_FALLING) {
                if (!(flags & (MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_SAFE_FALL))) {
                    if (state.fallVelocity == 0) {
                        if (state.fallTimeMs >= 500) state.flags = flags | MOVEMENTFLAG_FALLING_FAR;
                    } else if (!(double(state.fallStartZ) - 0.11111111f < state.position.z)) {
                        state.flags = flags | MOVEMENTFLAG_FALLING_FAR;
                    }
                }
                if ((state.flags & MOVEMENTFLAG_FALLING) && state.fallVelocity != 0 && (state.flags & 15)) {
                    state.currentSpeed = savedSpeed;
                    state.direction2D = savedDir2D;
                    state.direction = savedDir;
                }
                if (transport) SetTransport({transport});
            } else {
                if (transport) SetTransport({transport});
                if (HandlePendingActions()) return used;
            }
        }
        if (slid) {
            UpdateAnchors(0);
        }

        return used;
    }

    // 0x007618b0, with resident geometry and no attached transport frame.
    uint32_t Fall(uint32_t time, uint32_t elapsed, float distance, const C2Vector& direction) {
        if (!elapsed) return 0;
        const double fallen = RelDistanceFallen(elapsed + state.fallTimeMs);
        const glm::dvec3 move(double(direction.x)*distance,double(distance)*direction.y,-fallen);
        if (std::abs(std::sqrt((move.y*move.y+move.z*move.z)+move.x*move.x)) < 2.3841858e-7f) {
            state.fallTimeMs += elapsed;
            return elapsed;
        }
        uint32_t result;
        if (!facets.empty()) {
            result = FallDown(elapsed, C3Vector(move), true, time);
        } else {
            state.position = C3Vector(glm::dvec3(state.position) + move);
            state.fallTimeMs += elapsed;
            UpdateFallingFar();
            result = elapsed;
        }
        state.groundNormal = {0,0,1};
        return result;
    }

    // 0x007619c0. Save/RestoreMoveState restores anchors and direction, not
    // the speculative position or fall origin. This routine inspects that
    // endpoint; AttemptStepUp then restores the feet. Do not replace the
    // original snapshot with a wholesale copy/restore of the public state.
    int32_t WillPassObstacle(const C2Vector& direction, const C3Vector& position, float height) {
        uint32_t time = static_cast<uint32_t>(sqrt((height + height) * 0.051837362f) * 1000.0f);
        SurfaceMovementState saved;
        SaveMoveState(saved);
        StartFalling(0.0f);
        float fallen = RelDistanceFallen(time);
        C3Vector move(state.direction2D.x, state.direction2D.y, -fallen);

        for (uint32_t t = 0; t < time; t += FallDown(time - t, move)) {
            if (!(state.flags & MOVEMENTFLAG_FALLING)) {
                break;
            }
        }

        RestoreMoveState(saved);
        C2Vector from(position);
        C2Vector to(state.position);
        float dx = to.x - from.x;
        float dy = to.y - from.y;
        float length = sqrt(dy * dy + dx * dx);
        return length >= m_collisionRadius && dx * (1.0f / length) * direction.x + dy * (1.0f / length) * direction.y > 0.98480773f;
    }

    // 0x00761b00
    int32_t AttemptStepUp(const C2Vector& direction, C3Vector normal) {
        C3Vector saved = state.position;
        C4Plane planes[9];

        for (uint32_t i = 0; i < 7; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        C2Vector move = direction;
        float reach = m_collisionRadius + 0.0013888889f;

        if (reach < GetStepUpHeight() * 1.1917536f) {
            reach = GetStepUpHeight() * 1.1917536f;
        }

        uint32_t facetIndex;
        uint32_t planeCount;
        float moveDist;

        if (normal.z <= 0.64278764f && normal.z >= 0.0f) {
            double scale = 1.0 / sqrt(double(normal.x) * normal.x + double(normal.y) * normal.y);
            move.x = -(normal.x * scale);
            move.y = -(normal.y * scale);
            C3Vector probe(move.x, move.y, 0.0f);

            if (!DistanceToMove(probe, reach, &facetIndex, planes, &planeCount, &moveDist)) {
                return 0;
            }

            if (facetIndex == facets.size() || facets[facetIndex].plane.normal != normal) {
                move = direction;
            }
        }

        C3Vector up(0.0f, 0.0f, 1.0f);
        float upDist;

        if (!DistanceToMove(up, GetRemainingStepUpHeight(), &facetIndex, planes, &planeCount, &upDist)) {
            return 0;
        }

        float rise = upDist;

        if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
            rise = state.position.z - state.stepStartZ + upDist;
        }

        if (fabs(rise) < 0.00000023841858f) {
            state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            return 1;
        }

        state.position.z = upDist + state.position.z;
        C3Vector forward(move.x, move.y, 0.0f);

        if (!DistanceToMove(forward, reach, &facetIndex, planes, &planeCount, &moveDist)) {
            return 0;
        }

        state.position.x = move.x * moveDist + state.position.x;
        state.position.y = move.y * moveDist + state.position.y;

        int32_t cleared;

        if (fnotequal(moveDist, reach) && UseWalkableRedirection(facetIndex, &cleared)) {
            float remaining = reach - moveDist;
            C3Vector boxNormal{0};
            int32_t hasBox = GetBoxPushNormal(planes, planeCount, boxNormal);
            C3Vector moveDir(move.x, move.y, 0.0f);
            C3Vector push = CalcRunWalkWalkableObstaclePush(moveDir, &remaining, facets[facetIndex].plane.normal, hasBox, boxNormal);
            C3Vector slide(move.x * remaining + push.x, remaining * move.y + push.y, push.z);
            float length = sqrt(slide.x * slide.x + slide.y * slide.y + push.z * push.z);

            if (fnotequal(length, 0.0f)) {
                slide /= length;
                float slideDist;

                if (!DistanceToMove(slide, length, &facetIndex, planes, &planeCount, &slideDist)) {
                    return 0;
                }

                slide *= slideDist;
                C2Vector flat(slide);
                moveDist = sqrt(flat.y * flat.y + flat.x * flat.x) + moveDist;
                state.position += slide;
                upDist = slide.z + upDist;
            }
        }

        if (facetIndex != facets.size() && m_collisionRadius >= moveDist) {
            state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
            state.position = saved;
            return 1;
        }

        C3Vector down(0.0f, 0.0f, -1.0f);
        float downDist;

        if (!DistanceToMove(down, upDist, &facetIndex, planes, &planeCount, &downDist)) {
            return 0;
        }

        state.position.z = state.position.z - downDist;

        if (facetIndex == facets.size() || !IsSurfaceTooSteep(facetIndex) || WillPassObstacle(move, saved, upDist - downDist)) {
            state.position = saved;

            if (!(state.flags & MOVEMENTFLAG_SPLINE_ELEVATION)) {
                state.stepStartZ = saved.z;
            }

            state.flags |= MOVEMENTFLAG_SPLINE_ELEVATION;
            return 1;
        }

        state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
        state.position = saved;
        return 1;
    }

    // 0x007620f0
    uint32_t TraceSurface(uint32_t elapsed, float distance, const C2Vector& direction) {
        if (fabs(distance) < 0.00000095367432f) {
            return elapsed;
        }

        if (!facets.size()) {
            return StartFalling(0.0f) ? 0 : elapsed;
        }

        C4Plane planes[9];

        for (uint32_t i = 0; i < 7; i++) {
            planes[i].normal = C3Vector(0.0f, 0.0f, 1.0f);
            planes[i].offset = 0.0f;
        }

        float total = static_cast<float>(elapsed);
        float timeLeft = total;
        float timeUsed = 0.0f;
        int32_t stuck = 0;
        int32_t cleared = 0;
        int32_t moved = 0;
        float remaining = distance;
        float length2D = distance;
        C2Vector dir2D = direction;
        float climbBudget = distance / m_collisionRadius;
        C3Vector moveDir(direction.x, direction.y, 0.0f);
        uint32_t facetIndex;
        uint32_t planeCount;
        float moveDist;
        WOWGUID transport;

        if (!DistanceToMove(moveDir, distance, &facetIndex, planes, &planeCount, &moveDist)) {
            return elapsed;
        }

        while (true) {
            glm::dvec3 step=glm::dvec3(moveDir)*double(moveDist);

            if (climbBudget >= step.z) {
                climbBudget = climbBudget - step.z;
            } else {
                moved = 1;
                double scale = climbBudget / step.z;
                step.x = step.x * scale;
                step.y = step.y * scale;
                step.z = step.z * scale;
                moveDist = moveDist * scale;
                climbBudget = 0.0f;
            }

            state.position.x = step.x + state.position.x;
            state.position.y = step.y + state.position.y;
            state.position.z = step.z + state.position.z;

            if (facetIndex >= facets.size()) {
                const double used = double(timeLeft) + timeUsed;
                timeUsed = float(used);
                const double left = total - used;

                if (left >= 1.0f) {
                    timeLeft = left;
                    dir2D = direction;
                    remaining = left / total * distance;
                    moveDir = C3Vector(direction.x, direction.y, 0.0f);
                    goto next;
                }

                C3Vector down(0.0f, 0.0f, -1.0f);
                float downDist;

                if (!DistanceToMove(down, distance * 1.849399f, &facetIndex, planes, &planeCount, &downDist)) {
                    return elapsed;
                }

                state.position.z = state.position.z - downDist;

                if (facetIndex == facets.size()) {
                    transport = WOWGUID();

                    if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
                        goto finish;
                    }
                } else {
                    transport.m_guid = facets[facetIndex].owner;

                    if (!IsSurfaceTooSteep(facetIndex)) {
                        state.flags &= ~MOVEMENTFLAG_SPLINE_ELEVATION;
                        goto finish;
                    }

                    const CFacet& facet = facets[facetIndex];

                    if ((state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) && facet.plane.normal.y * dir2D.y + facet.plane.normal.x * dir2D.x <= -0.0000099999997f) {
                        goto finish;
                    }
                }

                goto fall;
            }

            {
                transport.m_guid = facets[facetIndex].owner;
                float timeStep = double(moveDist) / remaining * timeLeft;
                remaining = remaining - moveDist;
                C2Vector flat{C3Vector(step)};
                length2D = length2D - sqrt(double(flat.y) * flat.y + double(flat.x) * flat.x);

                if (timeStep < 1.0f) {
                    if (++stuck > 5) {
                        moved = 1;
                        goto finish;
                    }
                } else {
                    stuck = 1;
                }

                timeUsed = timeUsed + timeStep;
                timeLeft = timeLeft - timeStep;

                if (total - timeUsed < 1.0f) {
                    if (fnotequal(total, timeUsed)) {
                        moved = 1;
                    }

                    goto finish;
                }

                C3Vector normal = facets[facetIndex].plane.normal;
                C3Vector push{0};
                C3Vector boxNormal{0};
                int32_t hasBox;

                if (UseWalkableRedirection(facetIndex, &cleared)) {
                    hasBox = GetBoxPushNormal(planes, planeCount, boxNormal);
                } else {
                    if (cleared) {
                        goto fall;
                    }

                    moved = 1;
                    hasBox = GetBoxPushNormal(planes, planeCount, boxNormal);
                    uint32_t wasStepping = state.flags & MOVEMENTFLAG_SPLINE_ELEVATION;

                    if (!AttemptStepUp(dir2D, normal)) {
                        return elapsed;
                    }

                    if (!(state.flags & MOVEMENTFLAG_SPLINE_ELEVATION)) {
                        if (wasStepping) {
                            goto fall;
                        }

                        push = CalcRunWalkBlockingObstaclePush(moveDir, remaining, length2D, normal);
                        goto slide;
                    }
                }

                {
                    float before = remaining;
                    push = CalcRunWalkWalkableObstaclePush(moveDir, &remaining, normal, hasBox, boxNormal);
                    timeLeft = double(remaining) / before * timeLeft;
                }

            slide:

                glm::dvec3 next=glm::dvec3(moveDir)*double(remaining)+glm::dvec3(push);
                moveDir = next;
                remaining = sqrt(next.y * next.y + next.z * next.z + next.x * next.x);

                if (remaining < 0.001f) {
                    moved = 1;
                    goto finish;
                }

                dir2D = C2Vector(moveDir);
                double scale = 1.0 / remaining;
                moveDir.x = moveDir.x * scale;
                moveDir.y = moveDir.y * scale;
                moveDir.z = scale * moveDir.z;
                float previous = length2D;
                const double flatLength = sqrt(double(dir2D.y) * dir2D.y + double(dir2D.x) * dir2D.x);
                    length2D = float(flatLength);

                if (fabs(length2D) >= 0.00000023841858f) {
                    double inverse = 1.0 / flatLength;
                    dir2D.x = dir2D.x * inverse;
                    dir2D.y = dir2D.y * inverse;
                }

                if (previous - length2D > 0.00000095367432f) {
                    moved = 1;
                }

                if ((state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) && next.z > 0.00000095367432f) {
                    float top = float(next.z) + state.position.z;

                    if (GetStepUpHeight() + state.stepStartZ < top) {
                        double allowed = double(GetStepUpHeight()) + state.stepStartZ - state.position.z;
                        remaining = double(allowed) / float(next.z) * remaining;

                        if (fnotequal(allowed, next.z)) {
                            moved = 1;
                        }

                        if (remaining < 0.001f) {
                            goto fall;
                        }
                    }
                }
            }

        next:
            if (!DistanceToMove(moveDir, remaining, &facetIndex, planes, &planeCount, &moveDist)) {
                return elapsed;
            }
        }

    finish:
        if (cleared) {
            goto fall;
        }

        if (facetIndex >= facets.size()) {
            if (state.flags & MOVEMENTFLAG_SPLINE_ELEVATION) {
                return elapsed;
            }

            goto fall;
        }

        if (moved) {
            UpdateAnchors(0);
        }

        SetTransport(transport);
        return elapsed;

    fall:
        StartFalling(0.0f);
        return elapsed;
    }

    // 0x00986f00
    double CalcFallDistance(float inputTime, int32_t safeFall, float inputVelocity) const {
        const double time = inputTime;
        double velocity = inputVelocity;
        double terminal = safeFall ? s_safeFallTerminalVelocity : s_terminalVelocity;

        if (velocity > terminal) {
            velocity = terminal;
        }

        if (19.291105f * time + velocity > terminal) {
            double accelTime = (terminal - velocity) * 0.051837362f;
            return terminal * (time - accelTime) + (velocity + 9.6455526f * accelTime) * accelTime;
        }

        return time * (velocity + time * 9.6455526f);
    }

    // 0x00988220
    double TimeToFallDistance(float inputDistance, int32_t safeFall) const {
        const double distance = inputDistance;
        double terminal = safeFall ? s_safeFallTerminalVelocity : s_terminalVelocity;
        double accelTime = 0.051837362f * terminal;
        double accelDistance = accelTime * terminal * 0.5f;

        if (distance < accelDistance) {
            if (0.0f < distance) {
                return sqrt(distance * 0.103674725f);
            }

            return 0.0f;
        }

        return (distance - accelDistance) / terminal + accelTime;
    }

    // 0x009880c0
    void CalcCurrentSpeed(int32_t force) {
        if (!(state.flags & MOVEMENTFLAG_FALLING) || force) {
            state.currentSpeed = GetBaseSpeed(force);
        }
    }

    // 0x00987ef0
    void CalcDirection() {
        float facing = state.anchorFacing;
        C2Vector dir2D{0};
        dir2D.x = cos(facing);
        dir2D.y = sin(facing);
        state.direction2D = dir2D;

        if ((state.flags & (MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING)) && fabs(state.anchorPitch) >= 0.00000095367432f) {
            float pitch = state.anchorPitch;
            state.pitchCos = cos(pitch);
            state.pitchSin = sin(pitch);
            state.direction.x = state.pitchCos * dir2D.x;
            state.direction.y = state.pitchCos * dir2D.y;
            state.direction.z = state.pitchSin;
        } else {
            state.direction.x = state.direction2D.x;
            state.direction.y = state.direction2D.y;
            state.pitchSin = 0.0f;
            state.direction.z = 0.0f;
            state.pitchCos = 1.0f;
        }
    }

    // 0x00987ef0
    void CalcDirection(int32_t force) {
        if ((state.flags & MOVEMENTFLAG_FALLING) && !force) {
            return;
        }

        CalcDirection();
        uint32_t flags = state.flags;

        if (flags & (MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD)) {
            if (flags & (MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT)) {
                float forward2DX = state.direction2D.x;
                float forward2DY = state.direction2D.y;
                float forwardX = state.direction.x;
                float forwardY = state.direction.y;
                float forwardZ = state.direction.z;

                if (flags & MOVEMENTFLAG_BACKWARD) {
                    forward2DX = -forward2DX;
                    forward2DY = -forward2DY;
                    forwardX = -forwardX;
                    forwardY = -forwardY;
                    forwardZ = -forwardZ;
                }

                float swap = state.direction2D.x;
                state.direction2D.x = state.direction2D.y;
                state.direction2D.y = swap;

                if (state.flags & MOVEMENTFLAG_STRAFE_LEFT) {
                    state.direction2D.x = -state.direction2D.x;
                } else {
                    state.direction2D.y = -state.direction2D.y;
                }

                state.direction.x = state.direction2D.x + forwardX;
                state.direction.y = state.direction2D.y + forwardY;
                state.direction.z = forwardZ;
                state.direction2D.x = state.direction2D.x + forward2DX;
                state.direction2D.y = state.direction2D.y + forward2DY;
                state.direction.x = state.direction.x * 0.70710677f;
                state.direction.y = state.direction.y * 0.70710677f;
                state.direction.z = state.direction.z * 0.70710677f;
                state.direction2D.x = state.direction2D.x * 0.70710677f;
                state.direction2D.y = 0.70710677f * state.direction2D.y;
                return;
            }
        } else if (flags & (MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT)) {
            float swap = state.direction2D.x;
            state.direction2D.x = state.direction2D.y;
            state.direction2D.y = swap;

            if (state.flags & MOVEMENTFLAG_STRAFE_LEFT) {
                state.direction2D.x = -state.direction2D.x;
            } else {
                state.direction2D.y = -state.direction2D.y;
            }

            state.direction.x = state.direction2D.x;
            state.direction.y = state.direction2D.y;
            state.direction.z = 0.0f;
            return;
        }

        if (flags & MOVEMENTFLAG_BACKWARD) {
            state.direction = C3Vector(-state.direction.x, -state.direction.y, -state.direction.z);
            float x = -state.direction2D.x;
            float y = -state.direction2D.y;
            state.direction2D.x = x;
            state.direction2D.y = y;
        }
    }

    // 0x00988280
    double CalcTimeFallen(float distance, int32_t upward) const {
        double terminal = (state.flags & MOVEMENTFLAG_SAFE_FALL) ? s_safeFallTerminalVelocity : s_terminalVelocity;
        double velocity = state.fallVelocity;

        if (velocity > terminal) {
            velocity = terminal;
        }

        if (fabs(velocity) < 0.00000023841858f) {
            return TimeToFallDistance(distance, state.flags & MOVEMENTFLAG_SAFE_FALL);
        }

        double discriminant = velocity * velocity + double(38.582211f) * distance;
        double root = 0.0f;

        if (discriminant > 0.0f) {
            root = sqrt(discriminant);
        }

        double timeUp = (-velocity - root) * 0.051837362f;
        double timeDown = (root - velocity) * 0.051837362f;
        double accelTime = 0.051837362f * (terminal - velocity);
        double result;

        if (timeDown <= accelTime) {
            result = timeDown;
        } else {
            result = (distance - (velocity + accelTime * 9.6455526f) * accelTime) / terminal + accelTime;
        }

        if (upward) {
            result = 0.0f;

            if (timeUp >= 0.0f) {
                return timeUp;
            }
        }

        return result;
    }

    // 0x00987570
    float GetBaseSpeed(int32_t walk) const {
        uint32_t flags = state.flags;

        if (!(flags & (MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT | MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING))) {
            return 0.0f;
        }

        if (flags & MOVEMENTFLAG_FLYING) {
            if ((flags & MOVEMENTFLAG_BACKWARD) && state.flightSpeed >= state.flightBackSpeed) {
                return state.flightBackSpeed;
            }

            return state.flightSpeed;
        }

        if (flags & MOVEMENTFLAG_SWIMMING) {
            if ((flags & MOVEMENTFLAG_BACKWARD) && state.swimSpeed >= state.swimBackSpeed) {
                return state.swimBackSpeed;
            }

            return state.swimSpeed;
        }

        if ((flags & MOVEMENTFLAG_WALKING) || walk) {
            if (state.runSpeed > state.walkSpeed) {
                return state.walkSpeed;
            }
        } else if ((flags & MOVEMENTFLAG_BACKWARD) && state.runSpeed >= state.runBackSpeed) {
            return state.runBackSpeed;
        }

        return state.runSpeed;
    }

    // 0x009870d0
    double RelDistanceFallen(uint32_t time, float z) const {
        uint32_t flags = state.flags;
        float velocity = state.fallVelocity;

        if (flags & MOVEMENTFLAG_FALLING) {
            return CalcFallDistance(float(double(time) * 0.001f), flags & MOVEMENTFLAG_SAFE_FALL, velocity) + (double(z) - state.fallStartZ);
        }

        return CalcFallDistance(float(double(time) * 0.001f), flags & MOVEMENTFLAG_SAFE_FALL, velocity);
    }

    // 0x00988490
    void StopFalling() {
        uint32_t flags = state.flags;
        uint32_t falling = flags & MOVEMENTFLAG_FALLING;

        if (falling) {
            state.flags = flags & ~(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        }

        if (state.flags & MOVEMENTFLAG_PENDING_ROOT) {
            state.flags = (state.flags & 0xFF203700) | MOVEMENTFLAG_ROOT;
        } else if (!falling) {
            return;
        }

        UpdateAnchors(0);
        CalcCurrentSpeed(0);
    }

    // 0x00986e80
    double TimeToJumpPeak() const {
        if (state.flags & MOVEMENTFLAG_FALLING) {
            return double(state.fallVelocity) * -0.051837362f;
        }

        return 0.0f;
    }

    // 0x009881d0
    void UpdateAnchors(int32_t force) {
        state.anchorFacing = state.facing;
        state.anchorPosition = state.position;
        state.anchorPitch = state.pitch;
        state.anchorElapsedMs = 0;
        CalcDirection(force);
        
    }

    // 0x0075cf80
    void UpdateFallingFar() {
        uint32_t flags = state.flags;

        if (flags & (MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_SAFE_FALL)) {
            return;
        }

        if ((flags & MOVEMENTFLAG_FALLING) && 0.0f != state.fallVelocity) {
            if (state.fallStartZ - 0.11111111f >= state.position.z) {
                state.flags = flags | MOVEMENTFLAG_FALLING_FAR;
            }
        } else if (state.fallTimeMs >= 500) {
            state.flags = flags | MOVEMENTFLAG_FALLING_FAR;
        }
    }

    // 0x009870d0
    double RelDistanceFallen(uint32_t time) const {
        return RelDistanceFallen(time, state.position.z);
    }

    // 0x00988370
    int32_t StartFalling(float velocity) {
        if (state.flags & (MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_ROOT)) {
            return 0;
        }


        UpdateAnchors(0);
        state.flags = (state.flags & ~(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_SPLINE_ELEVATION)) | MOVEMENTFLAG_FALLING;

        if (!(state.flags2 & MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING)) {
            state.flags &= ~(MOVEMENTFLAG_PITCH_UP | MOVEMENTFLAG_PITCH_DOWN);
        }

        state.fallTimeMs = 0;
        state.fallStartZ = state.position.z;
        state.fallVelocity = velocity;
        return 1;
    }
};
} // namespace

SurfaceMovementResult traceMovementSurface(std::span<const MovementFacet> facets,
    SurfaceMovementState& state, const SurfaceMovementShape& shape,
    const glm::vec2& direction, float distance, uint32_t elapsedMs) {
    SurfaceTraversal traversal{facets, state, shape.radius, shape.height,
                               shape.stepHeight, shape.clientControlled, std::nullopt, std::nullopt};
    const auto used = traversal.TraceSurface(elapsedMs, distance, direction);
    return {used, traversal.transportRequest, traversal.fallResetTime};
}
SurfaceMovementResult traceMovementFall(std::span<const MovementFacet> facets,
    SurfaceMovementState& state, const SurfaceMovementShape& shape,
    const glm::vec2& direction, float distance, uint32_t timeMs, uint32_t elapsedMs) {
    SurfaceTraversal traversal{facets, state, shape.radius, shape.height,
                               shape.stepHeight, shape.clientControlled, std::nullopt, std::nullopt};
    const auto used = traversal.Fall(timeMs, elapsedMs, distance, direction);
    return {used, traversal.transportRequest, traversal.fallResetTime};
}
SurfaceMovementResult traceMovementSwim(std::span<const MovementFacet> solids,
    std::span<const MovementFacet> liquids, SurfaceMovementState& state,
    const SurfaceMovementShape& shape, const glm::vec3& direction,
    float distance, uint32_t elapsedMs) {
    SurfaceTraversal traversal{solids, state, shape.radius, shape.height,
                               shape.stepHeight, shape.clientControlled, std::nullopt, std::nullopt};
    const auto used = traversal.Swim(liquids, elapsedMs, distance, direction);
    return {used, traversal.transportRequest, traversal.fallResetTime};
}
SurfaceMovementResult traceMovementHover(std::span<const MovementFacet> facets,
    SurfaceMovementState& state, const SurfaceMovementShape& shape,
    const glm::vec3& direction, float distance, uint32_t timeMs, uint32_t elapsedMs) {
    SurfaceTraversal traversal{facets, state, shape.radius, shape.height,
                               shape.stepHeight, shape.clientControlled, std::nullopt, std::nullopt};
    const auto used = traversal.HoverMove(timeMs, elapsedMs, distance, direction);
    return {used, traversal.transportRequest, traversal.fallResetTime};
}
MovementFacetRequest makeMovementFacetRequest(const SurfaceMovementState& state,
    const SurfaceMovementShape& shape, const glm::vec3& direction, float distance,
    uint32_t elapsedMs, bool locallyControlled, bool ghost) {
    auto copy=state;
    SurfaceTraversal traversal{{}, copy, shape.radius, shape.height,
                               shape.stepHeight, shape.clientControlled, std::nullopt, std::nullopt};
    return traversal.GetMoveFacetRequest(direction,distance,elapsedMs,locallyControlled,ghost);
}
std::optional<MovementFacetRequest> expandMovementFacetRequest(
    const MovementFacetRequest& cached, const SurfaceMovementState& state,
    const SurfaceMovementShape& shape, const glm::vec3& displacement,
    bool locallyControlled, bool ghost) {
    auto copy=state;
    SurfaceTraversal traversal{{}, copy, shape.radius, shape.height,
                               shape.stepHeight, shape.clientControlled, std::nullopt, std::nullopt};
    auto result=traversal.BodyFacetRequest(locallyControlled,ghost);
    const auto low=result.bodyMin+displacement,high=result.bodyMax+displacement;
    if (glm::all(glm::greaterThanEqual(low,cached.queryMin)) &&
        glm::all(glm::lessThanEqual(low,cached.queryMax)) &&
        glm::all(glm::greaterThanEqual(high,cached.queryMin)) &&
        glm::all(glm::lessThanEqual(high,cached.queryMax))) return std::nullopt;
    result.queryMin=glm::min(low-glm::vec3(.16666667f),cached.queryMin);
    result.queryMax=glm::max(high+glm::vec3(.16666667f),cached.queryMax);
    return result;
}
MovementRequestResult traceUnattachedMovementRequest(MovementFacetSource& world,
    MovementRequestCache& cache,SurfaceMovementState& state,const SurfaceMovementShape& shape,
    const glm::vec3& displacement,uint32_t timeMs,uint32_t elapsedMs,
    bool locallyControlled,bool ghost) {
    RequestGeometry geometry{world,locallyControlled,ghost,cache,MovementGeometryStatus::Ready};
    SurfaceTraversal traversal{cache.solids,state,shape.radius,shape.height,shape.stepHeight,
        shape.clientControlled,std::nullopt,std::nullopt,&geometry};
    return traversal.CollideRequestMove(geometry,displacement,timeMs,elapsedMs);
}
} // namespace wowee::collision
